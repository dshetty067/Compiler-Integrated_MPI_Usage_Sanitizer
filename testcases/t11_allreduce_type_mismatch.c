/* Test 11: Type mismatch in Allreduce
 * BUG: some ranks use MPI_INT, others MPI_FLOAT — collective type must match */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    // All use the same buffer but different dtypes based on rank
    double buf = (double)rank;
    double result = 0.0;

    if (rank == 0) {
        int local = (int)buf;
        int res = 0;
        /* BUG: rank 0 uses MPI_INT while others use MPI_DOUBLE */
        MPI_Allreduce(&local, &res, 1, MPI_INT, MPI_SUM, MPI_COMM_WORLD);
        printf("[Rank 0] result=%d\n", res);
    } else {
        MPI_Allreduce(&buf, &result, 1, MPI_DOUBLE, MPI_SUM, MPI_COMM_WORLD);
        printf("[Rank %d] result=%f\n", rank, result);
    }

    MPI_Finalize();
    return 0;
}