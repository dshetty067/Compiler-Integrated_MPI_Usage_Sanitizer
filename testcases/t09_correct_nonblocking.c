/* Test 09: Correct non-blocking communication — should produce NO errors */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) { MPI_Finalize(); return 1; }

    double send_buf[4] = {1.1, 2.2, 3.3, 4.4};
    double recv_buf[4] = {0};
    MPI_Request req;

    if (rank == 0) {
        MPI_Isend(send_buf, 4, MPI_DOUBLE, 1, 10, MPI_COMM_WORLD, &req);
        MPI_Wait(&req, MPI_STATUS_IGNORE);
    } else if (rank == 1) {
        MPI_Irecv(recv_buf, 4, MPI_DOUBLE, 0, 10, MPI_COMM_WORLD, &req);
        MPI_Wait(&req, MPI_STATUS_IGNORE);
        printf("[Rank 1] recv_buf[0]=%f\n", recv_buf[0]);
    }

    MPI_Finalize();
    return 0;
}