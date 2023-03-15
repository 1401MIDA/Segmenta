Segmenta
-----------

Try the Current Version
=======================

NOTICE: the project is still in-progress. Try at your own risk, and this
section may be incomplete and subject to changes.

::

    # install from the repo, then
    git submodule update --init --recursive

    # ensure openssl and libevent are installed on your machine, more
    # specifically, you need:
    #
    # CMake >= 3.9 (cmake)
    # C++14 (g++)
    # libuv >= 1.10.0 (libuv1-dev)
    # openssl >= 1.1.0 (libssl-dev)
    #
    # on Ubuntu: sudo apt-get install libssl-dev libuv1-dev cmake make

    cmake -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED=ON -DHOTSTUFF_PROTO_LOG=ON
    make

    # start 4 demo replicas with scripts/run_demo.sh
    # then, start the demo client with scripts/run_demo_client.sh


    # Fault tolerance:
    # Try to run the replicas as in run_demo.sh first and then run_demo_client.sh.
    # Use Ctrl-C to terminate the proposing replica (e.g. replica 0). Leader
    # rotation will be scheduled. Try to kill and run run_demo_client.sh again, new
    # commands should still get through (be replicated) once the new leader becomes
    # stable. Or try the following script:
    # scripts/faulty_leader_demo.sh

Try to Reproduce Our Basic Results
==================================

See here_.

.. _here: https://github.com/1401MIDA/Segmenta/tree/master/scripts/deploy
