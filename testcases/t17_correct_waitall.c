/* Test 17: Correct Waitall with multiple requests — no bugs */
#include <mpi.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) { MPI_Finalize(); return 1; }

    int N = size - 1;
    double *bufs = (double*)malloc(N * sizeof(double));
    MPI_Request *reqs = (MPI_Request*)malloc(N * sizeof(MPI_Request));

    if (rank == 0) {
        for (int i = 0; i < N; i++) {
            bufs[i] = (double)(i + 1) * 1.5;
            MPI_Isend(&bufs[i], 1, MPI_DOUBLE, i + 1, i, MPI_COMM_WORLD, &reqs[i]);
        }
        MPI_Waitall(N, reqs, MPI_STATUSES_IGNORE);
        printf("[Rank 0] Sent %d messages via Waitall\n", N);
    } else {
        double val;
        MPI_Recv(&val, 1, MPI_DOUBLE, 0, rank - 1, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank %d] Received %f\n", rank, val);
    }

    free(bufs);
    free(reqs);
    MPI_Finalize();
    return 0;
}