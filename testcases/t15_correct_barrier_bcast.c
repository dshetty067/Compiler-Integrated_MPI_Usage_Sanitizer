/* Test 15: Correct barrier + broadcast — should produce NO errors */
#include <mpi.h>
#include <stdio.h>
#include <string.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    char msg[64];
    if (rank == 0) strncpy(msg, "Hello from rank 0", 63);

    MPI_Barrier(MPI_COMM_WORLD);
    MPI_Bcast(msg, 64, MPI_CHAR, 0, MPI_COMM_WORLD);

    printf("[Rank %d] msg='%s'\n", rank, msg);
    MPI_Finalize();
    return 0;
}