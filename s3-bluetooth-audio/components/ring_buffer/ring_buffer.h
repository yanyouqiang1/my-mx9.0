// s3-bluetooth-audio/components/ring_buffer/ring_buffer.h
#ifndef RING_BUFFER_H
#define RING_BUFFER_H

#include <stdint.h>
#include <stddef.h>

class RingBuffer {
public:
    RingBuffer(size_t capacity);
    ~RingBuffer();

    bool write(const uint8_t* data, size_t len);
    size_t read(uint8_t* data, size_t len);
    size_t available() const;
    void clear();

private:
    uint8_t* buffer_;
    size_t capacity_;
    size_t head_;
    size_t tail_;
    volatile size_t count_;
};

#endif
