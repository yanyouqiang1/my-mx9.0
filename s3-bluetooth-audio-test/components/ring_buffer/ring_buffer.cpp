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
    portENTER_CRITICAL(&mux_);
    if (len > capacity_ - count_) {
        portEXIT_CRITICAL(&mux_);
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        buffer_[head_] = data[i];
        head_ = (head_ + 1) % capacity_;
        count_++;
    }
    portEXIT_CRITICAL(&mux_);
    return true;
}

size_t RingBuffer::read(uint8_t* data, size_t len) {
    portENTER_CRITICAL(&mux_);
    size_t actual = (len < count_) ? len : count_;
    for (size_t i = 0; i < actual; i++) {
        data[i] = buffer_[tail_];
        tail_ = (tail_ + 1) % capacity_;
        count_--;
    }
    portEXIT_CRITICAL(&mux_);
    return actual;
}

size_t RingBuffer::available() const {
    portENTER_CRITICAL(&mux_);
    size_t result = count_;
    portEXIT_CRITICAL(&mux_);
    return result;
}
void RingBuffer::clear() {
    portENTER_CRITICAL(&mux_);
    head_ = tail_ = count_ = 0;
    portEXIT_CRITICAL(&mux_);
}
