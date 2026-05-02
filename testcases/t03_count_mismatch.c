/* Test 03: Count mismatch — receiver requests more data than sender provides
 * BUG: recv count > send count -> buffer overread */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    int buf[20] = {0};

    if (rank == 0) {
        for (int i = 0; i < 10; i++) buf[i] = i;
        /* Send only 10 ints */
        MPI_Send(buf, 10, MPI_INT, 1, 0, MPI_COMM_WORLD);
    } else if (rank == 1) {
        /* BUG: try to receive 20 ints — more than was sent */
        MPI_Recv(buf, 20, MPI_INT, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
        printf("[Rank 1] buf[0]=%d buf[15]=%d\n", buf[0], buf[15]);
    }

    MPI_Finalize();
    return 0;
}