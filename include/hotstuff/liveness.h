#ifndef _HOTSTUFF_LIVENESS_H
#define _HOTSTUFF_LIVENESS_H

#include "salticidae/util.h"
#include "hotstuff/consensus.h"

namespace hotstuff {

using salticidae::_1;
using salticidae::_2;

/** Abstraction for liveness gadget (oracle). */
class PaceMaker {
    protected:
    HotStuffCore *hsc;
    public:
    virtual ~PaceMaker() = default;
    /** Initialize the PaceMaker. A derived class should also call the
     * default implementation to set `hsc`. */
    virtual void init(HotStuffCore *_hsc) { hsc = _hsc; }
    /** Get a promise resolved when the pace maker thinks it is a *good* time
     * to issue new commands. When promise is resolved, the replica should
     * propose the command. */
    virtual promise_t beat() = 0;
    /** Get the current proposer. */
    virtual ReplicaID get_proposer() = 0;
    /** Select the parent blocks for a new block.
     * @return Parent blocks. The block at index 0 is the direct parent, while
     * the others are uncles/aunts. The returned vector should be non-empty. */
    virtual std::vector<block_t> get_parents() = 0;
    /** Get a promise resolved when the pace maker thinks it is a *good* time
     * to vote for a block. The promise is resolved with the next proposer's ID
     * */
    virtual promise_t beat_resp(ReplicaID last_proposer) = 0;
};

using pacemaker_bt = BoxObj<PaceMaker>;

/** Parent selection implementation for PaceMaker: select all parents.
 * PaceMakers derived from this class will select the highest block as the
 * direct parent, while including other tail blocks (up to parent_limit) as
 * uncles/aunts. */
class PMAllParents: public virtual PaceMaker {
    block_t bqc_tail;
    const int32_t parent_limit;         /**< maximum number of parents */
    
    void reg_bqc_update() {
        hsc->async_bqc_update().then([this](const block_t &bqc) {
            for (const auto &blk: hsc->get_tails())
            {
                block_t b;
                for (b = blk;
                    b->get_height() > bqc->get_height();
                    b = b->get_parents()[0]);
                if (b == bqc && blk->get_height() > bqc_tail->get_height())
                    bqc_tail = blk;
            }
            reg_bqc_update();
        });
    }

    void reg_proposal() {
        hsc->async_wait_proposal().then([this](const Proposal &prop) {
            if (prop.blk->get_parents()[0] == bqc_tail)
                bqc_tail = prop.blk;
            reg_proposal();
        });
    }

    void reg_receive_proposal() {
        hsc->async_wait_receive_proposal().then([this](const Proposal &prop) {
            if (prop.blk->get_parents()[0] == bqc_tail)
                bqc_tail = prop.blk;
            reg_receive_proposal();
        });
    }

    public:
    PMAllParents(int32_t parent_limit): parent_limit(parent_limit) {}
    void init() {
        bqc_tail = hsc->get_genesis();
        reg_bqc_update();
        reg_proposal();
        reg_receive_proposal();
    }

    std::vector<block_t> get_parents() override {
        const auto &tails = hsc->get_tails();
        std::vector<block_t> parents{bqc_tail};
        auto nparents = tails.size();
        if (parent_limit > 0)
            nparents = std::min(nparents, (size_t)parent_limit);
        nparents--;
        /* add the rest of tails as "uncles/aunts" */
        for (const auto &blk: tails)
        {
            if (blk != bqc_tail)
            {
                parents.push_back(blk);
                if (!--nparents) break;
            }
        }
        return std::move(parents);
    }
};

/** Beat implementation for PaceMaker: simply wait for the QC of last proposed
 * block.  PaceMakers derived from this class will beat only when the last
 * block proposed by itself gets its QC. */
class PMWaitQC: public virtual PaceMaker {
    std::queue<promise_t> pending_beats;
    block_t last_proposed;
    bool locked;

    protected:
    void schedule_next() {
        if (!pending_beats.empty() && !locked)
        {
            auto pm = pending_beats.front();
            pending_beats.pop();
            hsc->async_qc_finish(last_proposed).then([this, pm]() {
                pm.resolve(get_proposer());
            });
            locked = true;
        }
    }

    void update_last_proposed() {
        hsc->async_wait_proposal().then([this](const Proposal &prop) {
            update_last_proposed();
            last_proposed = prop.blk;
            locked = false;
            schedule_next();
        });
    }

    public:
    void init() {
        last_proposed = hsc->get_genesis();
        locked = false;
        update_last_proposed();
    }

    ReplicaID get_proposer() override {
        return hsc->get_id();
    }

    promise_t beat() override {
        promise_t pm;
        pending_beats.push(pm);
        schedule_next();
        return pm;
    }

    promise_t beat_resp(ReplicaID last_proposer) override {
        return promise_t([last_proposer](promise_t &pm) {
            pm.resolve(last_proposer);
        });
    }
};

/** Naive PaceMaker where everyone can be a proposer at any moment. */
struct PaceMakerDummy: public PMAllParents, public PMWaitQC {
    PaceMakerDummy(int32_t parent_limit):
        PMAllParents(parent_limit), PMWaitQC() {}
    void init(HotStuffCore *hsc) override {
        PaceMaker::init(hsc);
        PMAllParents::init();
        PMWaitQC::init();
    }
};

/** PaceMakerDummy with a fixed proposer. */
class PaceMakerDummyFixed: public PaceMakerDummy {
    ReplicaID proposer;

    public:
    PaceMakerDummyFixed(ReplicaID proposer,
                        int32_t parent_limit):
        PaceMakerDummy(parent_limit),
        proposer(proposer) {}

    ReplicaID get_proposer() override {
        return proposer;
    }

    promise_t beat_resp(ReplicaID) override {
        return promise_t([this](promise_t &pm) {
            pm.resolve(proposer);
        });
    }
};

/**
 * Simple long-standing proposer liveness gadget.
 * There are three roles for each replica: proposer, candidate and follower.
 *
 * For a proposer, it proposes a new block and refrains itself from proposing
 * the next block unless it receives the QC for the previous block. It will
 * give up the leadership and turn into a candidate when it hasn't seen such QC
 * for a while.
 *
 * For a follower, it never proposes any block, but keeps a timer for the QC
 * for the block last proposed by the proposer (the proposer it believes to
 * be). When it times out without seeing such QC, the follower turns into a
 * candidate.
 *
 * For a candidate, it periodically proposes empty blocks to synchronize the
 * preferred branch, with randomized timeout, and check for any new QC. Once it
 * sees such new QC, if the QC is given by itself, it becomes the proposer,
 * otherwise yields to the creator of the QC as a follower.
 */
class PMStickyProposer: virtual public PaceMaker {
    enum {
        PROPOSER,
        FOLLOWER,
        CANDIDATE
    } role;
    double qc_timeout;
    double candidate_timeout;
    EventContext ec;
    /** QC timer or randomized timeout */
    Event timer;
    block_t last_proposed;
    /** the proposer it believes */
    ReplicaID proposer;

    /* extra state needed for a proposer */
    std::queue<promise_t> pending_beats;
    bool locked;

    /* extra state needed for a candidate */
    std::unordered_map<ReplicaID, promise_t> last_proposed_by;

    promise_t pm_wait_receive_proposal;
    promise_t pm_wait_propose;
    promise_t pm_qc_finish;

    void reset_qc_timer() {
        timer.del();
        timer.add_with_timeout(qc_timeout);
        HOTSTUFF_LOG_PROTO("QC timer reset");
    }

    void clear_promises() {
        pm_wait_receive_proposal.reject();
        pm_wait_propose.reject();
        pm_qc_finish.reject();
        for (auto &p: last_proposed_by)
            p.second.reject();
        last_proposed_by.clear();
    }

    /* helper functions for a follower */

    void reg_follower_receive_proposal() {
        pm_wait_receive_proposal.reject();
        (pm_wait_receive_proposal = hsc->async_wait_receive_proposal())
            .then(
                salticidae::generic_bind(
                    &PMStickyProposer::follower_receive_proposal, this, _1));
    }

    void follower_receive_proposal(const Proposal &prop) {
        if (prop.proposer == proposer)
        {
            auto &qc_ref = prop.blk->get_qc_ref();
            if (last_proposed && qc_ref != last_proposed)
            {
                HOTSTUFF_LOG_PROTO("proposer misbehave");
                to_candidate(); /* proposer misbehave */
                return;
            }
            HOTSTUFF_LOG_PROTO("proposer emits new QC");
            last_proposed = prop.blk;
            reset_qc_timer();
        }
        reg_follower_receive_proposal();
    }

    /* helper functions for a proposer */

    void proposer_schedule_next() {
        if (!pending_beats.empty() && !locked)
        {
            auto pm = pending_beats.front();
            pending_beats.pop();
            pm_qc_finish.reject();
            (pm_qc_finish = hsc->async_qc_finish(last_proposed))
                .then([this, pm]() {
                    reset_qc_timer();
                    pm.resolve(proposer);
                });
            locked = true;
        }
    }

    void reg_proposer_propose() {
        pm_wait_propose.reject();
        (pm_wait_propose = hsc->async_wait_proposal()).then(
            salticidae::generic_bind(
                &PMStickyProposer::proposer_propose, this, _1));
    }

    void proposer_propose(const Proposal &prop) {
        last_proposed = prop.blk;
        locked = false;
        proposer_schedule_next();
        reg_proposer_propose();
    }

    void propose_elect_block() {
        DataStream s;
        /* FIXME: should extra data be the voter's id? */
        s << hsc->get_id();
        /* propose a block for leader election */
        hsc->on_propose(std::vector<command_t>{},
                get_parents(), std::move(s));
    }

    /* helper functions for a candidate */

    void candidate_qc_timeout() {
        pm_qc_finish.reject();
        pm_wait_propose.reject();
        (pm_wait_propose = hsc->async_wait_proposal()).then([this](const Proposal &prop) {
            const auto &blk = prop.blk;
            (pm_qc_finish = hsc->async_qc_finish(blk)).then([this, blk]() {
                HOTSTUFF_LOG_PROTO("collected QC for %s", std::string(*blk).c_str());
                /* managed to collect a QC */
                to_proposer();
                propose_elect_block();
            });
        });
        double t = salticidae::gen_rand_timeout(candidate_timeout);
        timer.del();
        timer.add_with_timeout(t);
        HOTSTUFF_LOG_PROTO("candidate next try in %.2fs", t);
        propose_elect_block();
    }

    void reg_candidate_receive_proposal() {
        pm_wait_receive_proposal.reject();
        (pm_wait_receive_proposal = hsc->async_wait_receive_proposal())
            .then(
                salticidae::generic_bind(
                    &PMStickyProposer::candidate_receive_proposal, this, _1));
    }

    void candidate_receive_proposal(const Proposal &prop) {
        auto _proposer = prop.proposer;
        auto &p = last_proposed_by[_proposer];
        HOTSTUFF_LOG_PROTO("got block %s from %d", std::string(*prop.blk).c_str(), _proposer);
        p.reject();
        (p = hsc->async_qc_finish(prop.blk)).then([this, _proposer]() {
            to_follower(_proposer);
        });
        reg_candidate_receive_proposal();
    }

    /* role transitions */

    void to_follower(ReplicaID new_proposer) {
        HOTSTUFF_LOG_PROTO("new role: follower");
        clear_promises();
        role = FOLLOWER;
        proposer = new_proposer;
        last_proposed = nullptr;
        hsc->set_neg_vote(false);
        timer = Event(ec, -1, 0, [this](int, short) {
            /* unable to get a QC in time */
            to_candidate();
        });
        /* redirect all pending cmds to the new proposer */
        while (!pending_beats.empty())
        {
            pending_beats.front().resolve(proposer);
            pending_beats.pop();
        }
        reg_follower_receive_proposal();
    }

    void to_proposer() {
        HOTSTUFF_LOG_PROTO("new role: proposer");
        clear_promises();
        role = PROPOSER;
        proposer = hsc->get_id();
        last_proposed = nullptr;
        hsc->set_neg_vote(true);
        timer = Event(ec, -1, 0, [this](int, short) {
            /* proposer unable to get a QC in time */
            to_candidate();
        });
        proposer_propose(Proposal(-1, uint256_t(), hsc->get_genesis(), nullptr));
        reset_qc_timer();
    }

    void to_candidate() {
        HOTSTUFF_LOG_PROTO("new role: candidate");
        clear_promises();
        role = CANDIDATE;
        proposer = hsc->get_id();
        last_proposed = nullptr;
        hsc->set_neg_vote(false);
        timer = Event(ec, -1, 0, [this](int, short) {
            candidate_qc_timeout();
        });
        candidate_timeout = qc_timeout;
        reg_candidate_receive_proposal();
        candidate_qc_timeout();
    }

    public:
    PMStickyProposer(double qc_timeout, const EventContext &ec):
        qc_timeout(qc_timeout), ec(ec) {}

    void init() { to_candidate(); }

    ReplicaID get_proposer() override {
        return proposer;
    }

    promise_t beat() override {
        if (role != FOLLOWER)
        {
            promise_t pm;
            pending_beats.push(pm);
            if (role == PROPOSER)
                proposer_schedule_next();
            return std::move(pm);
        }
        else
            return promise_t([proposer=proposer](promise_t &pm) {
                pm.resolve(proposer);
            });
    }

    promise_t beat_resp(ReplicaID last_proposer) override {
        return promise_t([this, last_proposer](promise_t &pm) {
            pm.resolve(last_proposer);
        });
    }
};

struct PaceMakerSticky: public PMAllParents, public PMStickyProposer {
    PaceMakerSticky(int32_t parent_limit, double qc_timeout, EventContext eb):
        PMAllParents(parent_limit), PMStickyProposer(qc_timeout, eb) {}

    void init(HotStuffCore *hsc) override {
        PaceMaker::init(hsc);
        PMAllParents::init();
        PMStickyProposer::init();
    }
};

}

#endif
