#include <stdio.h>
#include <stdint.h>

#define QR_SIZE 177

void test_positioning() {
    printf("QR Code Positioning Test\n");
    printf("========================\n\n");
    
    // Calculate positioning
    unsigned total_width = QR_SIZE + 8; // QR + quiet zone
    unsigned display_width = 320;
    unsigned left_margin = (display_width - total_width) / 2;
    unsigned qr_start_x = left_margin + 4; // +4 for quiet zone
    
    printf("QR size: %ux%u\n", QR_SIZE, QR_SIZE);
    printf("Total width (with quiet zone): %u\n", total_width);
    printf("Display width: %u\n", display_width);
    printf("Left margin: %u pixels\n", left_margin);
    printf("QR start X position: %u pixels\n\n", qr_start_x);
    
    // Test first few 8-bit chunks
    printf("First 8-bit chunk positioning:\n");
    for (unsigned i = 0; i < 3; ++i) {
        unsigned px = qr_start_x + i * 8u;
        unsigned byte_offset = px >> 3;
        unsigned bit_offset = px & 7;
        
        printf("Chunk %u: pixel %u, byte %u, bit %u\n", i, px, byte_offset, bit_offset);
        
        // Show which bytes and bits are affected
        if (bit_offset == 0) {
            printf("  -> Fits entirely in byte %u\n", byte_offset);
        } else {
            printf("  -> Spans bytes %u and %u\n", byte_offset, byte_offset + 1);
            printf("     Upper %u bits in byte %u, lower %u bits in byte %u\n",
                   8 - bit_offset, byte_offset, bit_offset, byte_offset + 1);
        }
    }
    
    printf("\nLast 8-bit chunk positioning:\n");
    unsigned last_chunk = 22; // 177 pixels / 8 = 22.125, so last chunk is index 22
    unsigned px = qr_start_x + last_chunk * 8u;
    unsigned byte_offset = px >> 3;
    unsigned bit_offset = px & 7;
    
    printf("Chunk %u: pixel %u, byte %u, bit %u\n", last_chunk, px, byte_offset, bit_offset);
    printf("  -> Only 1 pixel (last QR module)\n");
    
    // Verify total span
    unsigned first_byte = qr_start_x >> 3;
    unsigned last_pixel = qr_start_x + QR_SIZE - 1;
    unsigned last_byte = last_pixel >> 3;
    printf("\nQR code spans bytes %u to %u (inclusive)\n", first_byte, last_byte);
    printf("Total bytes used: %u\n", last_byte - first_byte + 1);
}

int main() {
    test_positioning();
    return 0;
}