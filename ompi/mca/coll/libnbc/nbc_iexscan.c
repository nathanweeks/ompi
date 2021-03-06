/* -*- Mode: C; c-basic-offset:4 ; indent-tabs-mode:nil -*- */
/*
 * Copyright (c) 2006      The Trustees of Indiana University and Indiana
 *                         University Research and Technology
 *                         Corporation.  All rights reserved.
 * Copyright (c) 2006      The Technical University of Chemnitz. All
 *                         rights reserved.
 * Copyright (c) 2013-2015 Los Alamos National Security, LLC. All rights
 *                         reserved.
 * Copyright (c) 2014-2017 Research Organization for Information Science
 *                         and Technology (RIST). All rights reserved.
 * Copyright (c) 2017      IBM Corporation.  All rights reserved.
 * $COPYRIGHT$
 *
 * Additional copyrights may follow
 *
 * Author(s): Torsten Hoefler <htor@cs.indiana.edu>
 *
 */
#include "nbc_internal.h"

#ifdef NBC_CACHE_SCHEDULE
/* tree comparison function for schedule cache */
int NBC_Scan_args_compare(NBC_Scan_args *a, NBC_Scan_args *b, void *param) {
    if ((a->sendbuf == b->sendbuf) &&
        (a->recvbuf == b->recvbuf) &&
        (a->count == b->count) &&
        (a->datatype == b->datatype) &&
        (a->op == b->op) ) {
        return 0;
    }

    if( a->sendbuf < b->sendbuf ) {
        return -1;
    }

    return 1;
}
#endif

/* linear iexscan
 * working principle:
 * 1. each node (but node 0) receives from left neigbor
 * 2. performs op
 * 3. all but rank p-1 do sends to it's right neigbor and exits
 *
 */
int ompi_coll_libnbc_iexscan(const void* sendbuf, void* recvbuf, int count, MPI_Datatype datatype, MPI_Op op,
                             struct ompi_communicator_t *comm, ompi_request_t ** request,
                             struct mca_coll_base_module_2_2_0_t *module) {
    int rank, p, res;
    ptrdiff_t gap, span;
    NBC_Schedule *schedule;
#ifdef NBC_CACHE_SCHEDULE
    NBC_Scan_args *args, *found, search;
#endif
    char inplace;
    void *tmpbuf = NULL;
    ompi_coll_libnbc_module_t *libnbc_module = (ompi_coll_libnbc_module_t*) module;

    NBC_IN_PLACE(sendbuf, recvbuf, inplace);

    rank = ompi_comm_rank (comm);
    p = ompi_comm_size (comm);

    span = opal_datatype_span(&datatype->super, count, &gap);
    if (0 < rank) {
        tmpbuf = malloc(span);
        if (NULL == tmpbuf) {
            return OMPI_ERR_OUT_OF_RESOURCE;
        }
        if (inplace) {
            res = NBC_Copy(recvbuf, count, datatype, (char *)tmpbuf-gap, count, datatype, comm);
        } else {
            res = NBC_Copy(sendbuf, count, datatype, (char *)tmpbuf-gap, count, datatype, comm);
        }
        if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
            free(tmpbuf);
            return res;
        }
    }

#ifdef NBC_CACHE_SCHEDULE
    /* search schedule in communicator specific tree */
    search.sendbuf = sendbuf;
    search.recvbuf = recvbuf;
    search.count = count;
    search.datatype = datatype;
    search.op = op;
    found = (NBC_Scan_args *) hb_tree_search ((hb_tree *) libnbc_module->NBC_Dict[NBC_EXSCAN], &search);
    if (NULL == found) {
#endif
        schedule = OBJ_NEW(NBC_Schedule);
        if (OPAL_UNLIKELY(NULL == schedule)) {
            free(tmpbuf);
            return OMPI_ERR_OUT_OF_RESOURCE;
        }

        if (rank != 0) {
            res = NBC_Sched_recv (recvbuf, false, count, datatype, rank-1, schedule, false);

            if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
                OBJ_RELEASE(schedule);
                free(tmpbuf);
                return res;
            }

            if (rank < p - 1) {
                /* we have to wait until we have the data */
                res = NBC_Sched_barrier(schedule);
                if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
                    OBJ_RELEASE(schedule);
                    free(tmpbuf);
                    return res;
                }

                res = NBC_Sched_op (recvbuf, false, (void *)(-gap), true, count,
                                     datatype, op, schedule, true);

                if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
                    OBJ_RELEASE(schedule);
                    free(tmpbuf);
                    return res;
                }

                /* send reduced data onward */
                res = NBC_Sched_send ((void *)(-gap), true, count, datatype, rank + 1, schedule, false);
                if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
                    OBJ_RELEASE(schedule);
                    free(tmpbuf);
                    return res;
                }
            }
        } else if (p > 1) {
            if (inplace) {
              res = NBC_Sched_send (recvbuf, false, count, datatype, 1, schedule, false);
            } else {
              res = NBC_Sched_send (sendbuf, false, count, datatype, 1, schedule, false);
            }
            if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
                OBJ_RELEASE(schedule);
                free(tmpbuf);
                return res;
            }
        }

        res = NBC_Sched_commit(schedule);
        if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
            OBJ_RELEASE(schedule);
            free(tmpbuf);
            return res;
        }

#ifdef NBC_CACHE_SCHEDULE
        /* save schedule to tree */
        args = (NBC_Scan_args *) malloc (sizeof (args));
        if (NULL != args) {
            args->sendbuf = sendbuf;
            args->recvbuf = recvbuf;
            args->count = count;
            args->datatype = datatype;
            args->op = op;
            args->schedule = schedule;
            res = hb_tree_insert ((hb_tree *) libnbc_module->NBC_Dict[NBC_EXSCAN], args, args, 0);
            if (0 == res) {
                OBJ_RETAIN(schedule);

                /* increase number of elements for A2A */
                if (++libnbc_module->NBC_Dict_size[NBC_EXSCAN] > NBC_SCHED_DICT_UPPER) {
                    NBC_SchedCache_dictwipe ((hb_tree *) libnbc_module->NBC_Dict[NBC_EXSCAN],
                                             &libnbc_module->NBC_Dict_size[NBC_EXSCAN]);
                }
            } else {
                NBC_Error("error in dict_insert() (%i)", res);
                free (args);
            }
        }
    } else {
        /* found schedule */
        schedule = found->schedule;
        OBJ_RETAIN(schedule);
    }
#endif

    res = NBC_Schedule_request(schedule, comm, libnbc_module, request, tmpbuf);
    if (OPAL_UNLIKELY(OMPI_SUCCESS != res)) {
        OBJ_RELEASE(schedule);
        free(tmpbuf);
        return res;
    }

    return OMPI_SUCCESS;
}
