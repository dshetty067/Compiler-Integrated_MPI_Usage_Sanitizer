/* Test 16: Float vs Int type confusion — subtle bug common in real code */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        float fval = 3.14f;
        /* BUG: using MPI_FLOAT (4 bytes) correctly for send */
        MPI_Send(&fval, 1, MPI_FLOAT, 1, 0, MPI_COMM_WORLD);
    } else if (rank == 1) {
        int ival = 0;
        /* BUG: receiving float data into int variable with MPI_INT */
        MPI_Recv(&ival, 1, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 1] Got int bits: %d (float bits of 3.14f = %d)\n",
               ival, ival);
    }

    MPI_Finalize();
    return 0;
}