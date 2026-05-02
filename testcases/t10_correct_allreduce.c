/* Test 10: Correct Allreduce — should produce NO errors */
#include <mpi.h>
#include <stdio.h>

int main(int argc, char **argv) {
    MPI_Init(&argc, &argv);
    int rank;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);

    double local_val  = (double)(rank + 1);
    double global_sum = 0.0;

    MPI_Allreduce(&local_val, &global_sum, 1, MPI_DOUBLE,
                  MPI_SUM, MPI_COMM_WORLD);

    printf("[Rank %d] local=%f global_sum=%f\n", rank, local_val, global_sum);
    MPI_Finalize();
    return 0;
}