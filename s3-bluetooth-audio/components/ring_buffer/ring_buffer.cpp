// s3-bluetooth-audio/components/ring_buffer/ring_buffer.cpp
#include "ring_buffer.h"
#include <string.h>

RingBuffer::RingBuffer(size_t capacity) : capacity_(capacity), head_(0), tail_(0), count_(0) {
    buffer_ = new uint8_t[capacity];
}

RingBuffer::~RingBuffer() {
    delete[] buffer_;
}

bool RingBuffer::write(const uint8_t* data, size_t len) {
    if (len > capacity_ - count_) return false;

    for (size_t i = 0; i < len; i++) {
        buffer_[head_] = data[i];
        head_ = (head_ + 1) % capacity_;
        count_++;
    }
    return true;
}

size_t RingBuffer::read(uint8_t* data, size_t len) {
    size_t actual = (len < count_) ? len : count_;
    for (size_t i = 0; i < actual; i++) {
        data[i] = buffer_[tail_];
        tail_ = (tail_ + 1) % capacity_;
        count_--;
    }
    return actual;
}

size_t RingBuffer::available() const { return count_; }
void RingBuffer::clear() { head_ = tail_ = count_ = 0; }
