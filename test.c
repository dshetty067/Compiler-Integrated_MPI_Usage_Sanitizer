#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);

    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {

        int value = 100;

        // Sending ONLY 1 integer
        MPI_Send(&value, 1, MPI_INT, 1, 0, MPI_COMM_WORLD);

    } else if (rank == 1) {

        int buffer[2];

        // Expecting 2 integers
        MPI_Recv(buffer, 1, MPI_DOUBLE, 0, 0,
                 MPI_COMM_WORLD, MPI_STATUS_IGNORE);

        printf("Received: %d %d\n", buffer[0], buffer[1]);
    }

    MPI_Finalize();
    return 0;
}