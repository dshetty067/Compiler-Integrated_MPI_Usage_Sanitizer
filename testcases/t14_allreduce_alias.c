/* Test 14: Buffer overlap — sendbuf and recvbuf point to same array
 * (MPI_IN_PLACE is valid but using the same pointer directly is not) */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    double data[4] = {1.0, 2.0, 3.0, 4.0};

    /* BUG: sending and receiving into same buffer (not using MPI_IN_PLACE) */
    MPI_Allreduce(data, data, 4, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);

    if (rank == 0)
        printf("[Rank 0] data[0]=%f (should be nprocs*1.0)\n", data[0]);

    MPI_Finalize();
    return 0;
}