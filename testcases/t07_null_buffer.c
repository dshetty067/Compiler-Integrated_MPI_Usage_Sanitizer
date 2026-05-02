/* Test 07: NULL buffer passed to MPI_Send
 * BUG: buf is NULL which is invalid for non-zero count */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    if (rank == 0) {
        /* BUG: NULL buffer with count > 0 */
        MPI_Send(NULL, 10, MPI_INT, 1, 0, MPI_COMM_WORLD);
    } else if (rank == 1) {
        int buf[10];
        MPI_Recv(buf, 10, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    MPI_Finalize();
    return 0;
}