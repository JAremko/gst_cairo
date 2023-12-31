#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <sys/file.h>
#include <sys/mman.h>

#define OVERLAY_FILE "/tmp/overlay"
#define PATTERN_SIZE_W 300  // Size of the square pattern
#define PATTERN_SIZE_H 250  // Size of the square pattern
#define UPDATE_INTERVAL 40000 // Update interval in microseconds

// Global variables to track the position and color of the box
static int box_x = 0;
static int box_y = 0;
static int box_width = 100; // Width of the box
static int box_height = 100; // Height of the box
static unsigned char box_red = 0;
static unsigned char box_green = 0;
static unsigned char box_blue = 0;

void draw_pattern(unsigned char *data, int width, int height) {

    // Offset the data pointer to skip the first byte (dirty flag)
    unsigned char *draw_data = data + 1;

    // Clear the buffer (set it to transparent or a background color)
    memset(draw_data, 0, width * height * 4);

    // Update the position of the box
    box_x = (box_x + 1) % (width - box_width);
    box_y = (box_y + 1) % (height - box_height);

    // Update the color of the box
    box_red = (box_red + 1) % 256;
    box_green = (box_green + 2) % 256;
    box_blue = (box_blue + 3) % 256;

    // Draw the box
    for (int y = box_y; y < box_y + box_height; ++y) {
        for (int x = box_x; x < box_x + box_width; ++x) {
            int index = (y * width + x) * 4;
            draw_data[index] = box_red;     // Red channel
            draw_data[index + 1] = box_green; // Green channel
            draw_data[index + 2] = box_blue;  // Blue channel
            draw_data[index + 3] = 225;      // Alpha channel
        }
    }

    *data = 1;
}

int main() {
    int fd;
    unsigned char *data;
    size_t length = 1 + PATTERN_SIZE_W * PATTERN_SIZE_H * 4; // 1 byte for the dirty flag

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
        if (flock(fd, LOCK_EX) == -1) {
            perror("Error loking file");
            close(fd);
            return EXIT_FAILURE;
        }

        // Draw the pattern
        draw_pattern(data, PATTERN_SIZE_W, PATTERN_SIZE_H);

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

        usleep(UPDATE_INTERVAL); // Wait for a while before the next update
    }

    // Cleanup: Unmap and close file
    munmap(data, length);
    close(fd);

    return 0;
}
