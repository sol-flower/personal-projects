/*
    Parallel Sorting using Regular Sampling (PSRS) using MPI cluster on a hypercube topology & comparison with best sequential sorting algorithm.

    Compile and run with:
        mpicc psrs.c -o q2 -lm
        mpirun -np 4 ./q2 5 2 8 1 9 3 7 4 6 10 12 11

    ** In order to run this file, a connection to an MPI environment is required. **
*/

#include <mpi.h>
#include <stdio.h>   
#include <stdlib.h>
#include <string.h>
#include <math.h>

int compare(const void *a, const void *b)
{
    return (*(int *)a - *(int *)b);
}

int* quicksort_sampling(int *local_data, int *local_size, int n, int size, int rank, MPI_Comm comm)
{
    int n_local = *local_size;

    //Step 1: Local quicksort
    qsort(local_data, *local_size, sizeof(int), compare);

    int *sample = malloc(size * sizeof(int));  //array to store samples
    int *all_samples = NULL;
    // Step 2: Regular sampling
    for(int i = 0; i < size - 1; i++) {
        sample[i] = local_data[(i * n_local) / size * size];
    }
    if(rank == 0){
        all_samples = malloc(size * size * sizeof(int));
    }
    MPI_Gather(sample, size, MPI_INT, all_samples, size, MPI_INT, 0, comm); //send all samples to process 0

    // Step 3: Choose pivots and broadcast
    int *pivots = malloc((size - 1) * sizeof(int)); //array to store pivots
    if(rank == 0){
        qsort(all_samples, size * size, sizeof(int), compare); //sort all samples
        for (int i = 0; i < size - 1; i++){
            pivots[i] = all_samples[(i + 1) * size + size / 2 - 1]; //select pivots
        }
        free(all_samples);
    }
    MPI_Bcast(pivots, size - 1, MPI_INT, 0, comm); //broadcast pivots to all processes

    // Step 4: Bucket distribution and exchange using All-to-All
    int **buckets = malloc(size * sizeof(int*));    //2D array to hold buckets
    int *bucket_sizes = calloc(size, sizeof(int));

    for (int i = 0; i < n_local; i++) { //count elements in each bucket
        int bucket = 0;
        while (bucket < size - 1 && local_data[i] > pivots[bucket]) {
            bucket++;
        }
        bucket_sizes[bucket]++;
    }

    for (int i = 0; i < size; i++) { //allocate memory for each bucket
        buckets[i] = malloc(bucket_sizes[i] * sizeof(int));
    }

    int *bucket_indices = calloc(size, sizeof(int));    
    for (int i = 0; i < n_local; i++) { //distribute elements into buckets
        int bucket = 0;
        while (bucket < size - 1 && local_data[i] > pivots[bucket]) {
            bucket++;
        }
        buckets[bucket][bucket_indices[bucket]++] = local_data[i]; // buckets[which bucket][data in that bucket]
    }
    free(bucket_indices);
    free(sample);
    free(pivots);

    //All to all communication to exchange buckets
    int *recv_counts = malloc(size * sizeof(int));
    MPI_Alltoall(bucket_sizes, 1, MPI_INT, recv_counts, 1, MPI_INT, comm);  //exchange bucket sizes

    int total_recv = 0, total_send = 0;
    for (int i = 0; i < size; i++) {    //total and receive sizes
        total_recv += recv_counts[i];
        total_send += bucket_sizes[i];
    }
    // Squash all send/receive buckets into single buffers and prepare displacements
    int *send_buffer = malloc(total_send * sizeof(int));
    int *send_displ = malloc(size * sizeof(int));
    int *recv_buffer = malloc(total_recv * sizeof(int));
    int *recv_displ = malloc(size * sizeof(int));
    
    int offset = 0;
    for (int i = 0; i < size; i++) {
        send_displ[i] = offset;
        memcpy(send_buffer + offset, buckets[i], bucket_sizes[i] * sizeof(int)); //bucket squashing
        offset += bucket_sizes[i];
        free(buckets[i]);
    }
    free(buckets);
    
    offset = 0;
    for (int i = 0; i < size; i++) {
        recv_displ[i] = offset;
        offset += recv_counts[i];
    }
    
    // All-to-all exchange of data
    MPI_Alltoallv(send_buffer, bucket_sizes, send_displ, MPI_INT,
                  recv_buffer, recv_counts, recv_displ, MPI_INT, comm);
    
    free(send_buffer);
    free(bucket_sizes);
    free(send_displ);
    free(recv_counts);
    free(recv_displ);

    // Step 5: Final local sort
    qsort(recv_buffer, total_recv, sizeof(int), compare);

    free(local_data);
    *local_size = total_recv;

    return recv_buffer;
}

int main(int argc, char *argv[])
{
    MPI_Init(&argc, &argv);

    int rank, size;
    MPI_Comm_rank(MPI_COMM_WORLD, &rank);
    MPI_Comm_size(MPI_COMM_WORLD, &size);

    int n = argc - 1; // Take all CLI args without program name

    if (n % size != 0) { // Check divisibility
        if (rank == 0) {
            printf("Error: Number of elements (%d) must be divisible by number of processes (%d)\n", n, size);
        }
        MPI_Finalize();
        return 1;
    }

    int local_size = n / size; //account for upper limit
    int *local_data = malloc(local_size * sizeof(int));

    int *global_data = NULL; // Process 0 reads all elements from command line and scatters them
    if (rank == 0)
    {
        global_data = malloc(n * sizeof(int));
        printf("Input array is made of %d elements: ", n);
        for (int i = 0; i < n; i++)
        {
            global_data[i] = atoi(argv[i + 1]);
            printf("%d ", global_data[i]);
        }
        printf("\n");
    }

    // Scatter the data to all processes
    MPI_Scatter(global_data, local_size, MPI_INT, local_data, local_size, MPI_INT, 0, MPI_COMM_WORLD);

    double start = MPI_Wtime();
    int *sorted_data = quicksort_sampling(local_data, &local_size, n, size, rank, MPI_COMM_WORLD);
    double end = MPI_Wtime();

    if (rank == 0)
    {
        printf("Parallel algo time using PSRS:  %.6f seconds\n", end - start);

        printf("Now performing sequential quicksort on given data...\n");   //calculate sequential quicksort time for comparison
        double seq_start = MPI_Wtime();
        qsort(global_data, n, sizeof(int), compare);
        double seq_end = MPI_Wtime();
        printf("Sequential algo time: sorted %d elements in %.6f seconds\n", n, seq_end - seq_start);

        free(global_data);
    }

    free(sorted_data);
    MPI_Finalize();
    return 0;
}