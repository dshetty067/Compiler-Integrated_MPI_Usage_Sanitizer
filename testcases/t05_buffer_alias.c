/* Test 05: Buffer aliasing — same buffer used for overlapping Isend+Irecv
 * BUG: buf is in-flight on Isend request when reused for Irecv */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    if (size < 2) { MPI_Finalize(); return 1; }

    double shared_buf[10];
    MPI_Request req1, req2;

    for (int i = 0; i < 10; i++) shared_buf[i] = (double)(rank * 10 + i);

    if (rank == 0) {
        MPI_Isend(shared_buf, 10, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, &req1);
        /* BUG: reuse shared_buf while req1 is still in flight */
        MPI_Irecv(shared_buf, 10, MPI_DOUBLE, 1, 1, MPI_COMM_WORLD, &req2);
        MPI_Wait(&req1, MPI_STATUS_IGNORE);
        MPI_Wait(&req2, MPI_STATUS_IGNORE);
    } else if (rank == 1) {
        MPI_Recv(shared_buf, 10, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        MPI_Send(shared_buf, 10, MPI_DOUBLE, 0, 1, MPI_COMM_WORLD);
    }

    MPI_Finalize();
    return 0;
}