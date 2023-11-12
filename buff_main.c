#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>

#define OVERLAY_FILE "/tmp/overlay"
#define PATTERN_SIZE_W 800  // Size of the square pattern
#define PATTERN_SIZE_H 600  // Size of the square pattern
#define UPDATE_INTERVAL 10000 // Update interval in microseconds

// Global variables to track color and opacity changes
static unsigned char red = 0;
static unsigned char alpha = 0;

void draw_pattern(unsigned char *data, int width, int height) {
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            int index = (y * width + x) * 4;
            data[index] = red;       // Red channel
            data[index + 1] = 0;     // Green channel
            data[index + 2] = 0;     // Blue channel
            data[index + 3] = alpha; // Alpha channel
        }
    }

    // Update color and opacity for next iteration
    red = (red + 1) % 256;
    alpha = (alpha + 1) % 256;
}

int main() {
    int fd;
    unsigned char *data;
    size_t length = PATTERN_SIZE_W * PATTERN_SIZE_H * 4; // Assuming ARGB32 format

    // Open and lock the file once outside the loop
    fd = open(OVERLAY_FILE, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        perror("Error opening/creating overlay file");
        return EXIT_FAILURE;
    }

    if (flock(fd, LOCK_EX) == -1) {
        perror("Error locking file");
        close(fd);
        return EXIT_FAILURE;
    }

    // Resize the file to the appropriate size once
    if (ftruncate(fd, length) == -1) {
        perror("Error setting file size");
        close(fd);
        return EXIT_FAILURE;
    }

    // Map the file into memory once
    data = (unsigned char *)mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        perror("Error mmapping the file");
        close(fd);
        return EXIT_FAILURE;
    }

    if (flock(fd, LOCK_UN) == -1) {
        perror("Error unloking file");
        close(fd);
        return EXIT_FAILURE;
    }

    while (1) {
        // Draw the pattern
        draw_pattern(data, PATTERN_SIZE_W, PATTERN_SIZE_H);

        if (flock(fd, LOCK_EX) == -1) {
            perror("Error loking file");
            close(fd);
            return EXIT_FAILURE;
        }

        // Synchronize the memory with the file system
        if (msync(data, length, MS_SYNC) == -1) {
            perror("Error syncing mmap");
            munmap(data, length);
            close(fd);
            return EXIT_FAILURE;
        }

        if (flock(fd, LOCK_UN) == -1) {
            perror("Error unloking file");
            close(fd);
            return EXIT_FAILURE;
        }

        printf("Iteration!\n");

        usleep(UPDATE_INTERVAL); // Wait for a while before the next update
    }

    // Cleanup: Unmap and close file
    munmap(data, length);
    close(fd);

    return 0;
}
