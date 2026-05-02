/* Test 01: Correct ping-pong - should produce NO errors */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) {
        fprintf(stderr, "Need at least 2 processes\n");
        MPI_Finalize();
        return 1;
    }

    double buf = 3.14;
    if (rank == 0) {
        MPI_Send(&buf, 1, MPI_DOUBLE, 1, 42, MPI_COMM_WORLD);
        MPI_Recv(&buf, 1, MPI_DOUBLE, 1, 43, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 0] Received: %f\n", buf);
    } else if (rank == 1) {
        MPI_Recv(&buf, 1, MPI_DOUBLE, 0, 42, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        buf *= 2.0;
        MPI_Send(&buf, 1, MPI_DOUBLE, 0, 43, MPI_COMM_WORLD);
    }

    MPI_Finalize();
    return 0;
}