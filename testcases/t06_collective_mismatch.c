/* Test 06: Collective ordering violation
 * BUG: rank 0 calls MPI_Bcast, rank 1 calls MPI_Barrier on same comm
 * This is undefined behavior in MPI */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int buf = rank;

    if (rank == 0) {
        /* BUG: rank 0 does Bcast but rank 1 does Barrier — mismatch */
        MPI_Bcast(&buf, 1, MPI_INT, 0, MPI_COMM_WORLD);
    } else {
        /* BUG: wrong collective on this rank */
        MPI_Barrier(MPI_COMM_WORLD);
    }

    printf("[Rank %d] buf=%d\n", rank, buf);
    MPI_Finalize();
    return 0;
}