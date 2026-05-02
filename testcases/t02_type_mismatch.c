/* Test 02: Type mismatch — sender uses MPI_INT, receiver uses MPI_DOUBLE
 * BUG: Different MPI datatypes on each end -> MPISAN should flag this */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        int data = 42;
        /* BUG: sending as MPI_INT */
        MPI_Send(&data, 1, MPI_INT, 1, 0, MPI_COMM_WORLD);
    } else if (rank == 1) {
        double buf = 0.0;
        /* BUG: receiving as MPI_DOUBLE (8 bytes vs 4 bytes) */
        MPI_Recv(&buf, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 1] Received as double: %f\n", buf);
    }

    MPI_Finalize();
    return 0;
}