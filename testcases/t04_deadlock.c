/* Test 04: Classic deadlock — both ranks do blocking Send before Recv
 * BUG: rank 0 waits for rank 1 to recv, rank 1 waits for rank 0 to recv */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    double buf = (double)rank;

    if (rank == 0) {
        /* BUG: blocking send before recv — if rank 1 does the same, deadlock */
        MPI_Send(&buf, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD);
        MPI_Recv(&buf, 1, MPI_DOUBLE, 1, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    } else if (rank == 1) {
        /* BUG: rank 1 also sends first — classic deadlock pattern */
        MPI_Send(&buf, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD);
        MPI_Recv(&buf, 1, MPI_DOUBLE, 0, 0, MPI_COMM_WORLD, MPI_STATUS_IGNORE);
    }

    printf("[Rank %d] Got %f\n", rank, buf);
    MPI_Finalize();
    return 0;
}