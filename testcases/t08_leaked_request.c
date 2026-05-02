/* Test 08: Leaked MPI_Request — Isend/Irecv without MPI_Wait
 * BUG: request is never waited on → resource leak, potential data corruption */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) { MPI_Finalize(); return 1; }

    double buf[5] = {1.0, 2.0, 3.0, 4.0, 5.0};
    MPI_Request req;

    if (rank == 0) {
        MPI_Isend(buf, 5, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &req);
        /* BUG: forgot to call MPI_Wait(&req, ...) before Finalize */
        printf("[Rank 0] Sent (but didn't wait!)\n");
    } else if (rank == 1) {
        MPI_Recv(buf, 5, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 1] Received\n");
    }

    MPI_Finalize();
    return 0;
}