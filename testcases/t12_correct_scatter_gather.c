/* Test 12: Correct Scatter/Gather — should produce NO errors */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int *sendbuf = NULL;
    if (rank == 0) {
        sendbuf = (int*)malloc(size * sizeof(int));
        for (int i = 0; i < size; i++) sendbuf[i] = i * 10;
    }

    int recvbuf = 0;
    MPI_Scatter(sendbuf, 1, MPI_INT, &recvbuf, 1, MPI_INT, 0, MPI_COMM_WORLD);
    printf("[Rank %d] Scattered value: %d\n", rank, recvbuf);

    int *gathered = NULL;
    if (rank == 0) {
        gathered = (int*)malloc(size * sizeof(int));
    }
    MPI_Gather(&recvbuf, 1, MPI_INT, gathered, 1, MPI_INT, 0, MPI_COMM_WORLD);
    if (rank == 0) printf("[Rank 0] Gathered[0]=%d\n", gathered[0]);

    free(sendbuf);
    free(gathered);
    MPI_Finalize();
    return 0;
}
