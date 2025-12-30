#pragma once

// The Arrow C data interface.
// https://arrow.apache.org/docs/format/CDataInterface.html

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef ARROW_C_DATA_INTERFACE
#define ARROW_C_DATA_INTERFACE

#define ARROW_FLAG_DICTIONARY_ORDERED 1
#define ARROW_FLAG_NULLABLE 2
#define ARROW_FLAG_MAP_KEYS_SORTED 4

struct ArrowSchema {
    // Array type description
    const char* format;
    const char* name;
    const char* metadata;
    int64_t flags;
    int64_t n_children;
    struct ArrowSchema** children;
    struct ArrowSchema* dictionary;

    // Release callback
    void (*release)(struct ArrowSchema*);
    // Opaque producer-specific data
    void* private_data;
};

struct ArrowArray {
    // Array data description
    int64_t length;
    int64_t null_count;
    int64_t offset;
    int64_t n_buffers;
    int64_t n_children;
    const void** buffers;
    struct ArrowArray** children;
    struct ArrowArray* dictionary;

    // Release callback
    void (*release)(struct ArrowArray*);
    // Opaque producer-specific data
    void* private_data;
};

#endif // ARROW_C_DATA_INTERFACE

#ifdef __cplusplus
}
#endif

struct ArrowSchemaWrapper : public ArrowSchema {
    ArrowSchemaWrapper() : ArrowSchema{} { release = nullptr; }
    ~ArrowSchemaWrapper() {
        if (release) {
            release(this);
        }
    }

    // Move constructor
    ArrowSchemaWrapper(ArrowSchemaWrapper&& other) noexcept : ArrowSchema(other) {
        other.release = nullptr;
    }

    // Move assignment
    ArrowSchemaWrapper& operator=(ArrowSchemaWrapper&& other) noexcept {
        if (this != &other) {
            if (release) {
                release(this);
            }
            ArrowSchema::operator=(other);
            other.release = nullptr;
        }
        return *this;
    }

    // Delete copy constructor and copy assignment
    ArrowSchemaWrapper(const ArrowSchemaWrapper&) = delete;
    ArrowSchemaWrapper& operator=(const ArrowSchemaWrapper&) = delete;
};

struct ArrowArrayWrapper : public ArrowArray {
    ArrowArrayWrapper() : ArrowArray{} { release = nullptr; }
    ~ArrowArrayWrapper() {
        if (release) {
            release(this);
        }
    }

    // Move constructor
    ArrowArrayWrapper(ArrowArrayWrapper&& other) noexcept : ArrowArray(other) {
        other.release = nullptr;
    }

    // Move assignment
    ArrowArrayWrapper& operator=(ArrowArrayWrapper&& other) noexcept {
        if (this != &other) {
            if (release) {
                release(this);
            }
            ArrowArray::operator=(other);
            other.release = nullptr;
        }
        return *this;
    }

    // Delete copy constructor and copy assignment
    ArrowArrayWrapper(const ArrowArrayWrapper&) = delete;
    ArrowArrayWrapper& operator=(const ArrowArrayWrapper&) = delete;
};

// Helper functions for creating shallow copies of Arrow wrappers
// These create copies that reference existing data without taking ownership
inline ArrowSchemaWrapper createShallowCopy(const ArrowSchemaWrapper& original) {
    ArrowSchemaWrapper copy;
    copy.format = original.format;
    copy.name = original.name;
    copy.metadata = original.metadata;
    copy.flags = original.flags;
    copy.n_children = original.n_children;
    copy.children = original.children;
    copy.dictionary = original.dictionary;
    copy.release = nullptr; // Don't release - original owns it
    copy.private_data = original.private_data;
    return copy;
}

inline ArrowArrayWrapper createShallowCopy(const ArrowArrayWrapper& original) {
    ArrowArrayWrapper copy;
    copy.length = original.length;
    copy.null_count = original.null_count;
    copy.offset = original.offset;
    copy.n_buffers = original.n_buffers;
    copy.n_children = original.n_children;
    copy.buffers = original.buffers;
    copy.children = original.children;
    copy.dictionary = original.dictionary;
    copy.release = nullptr; // Don't release - original owns it
    copy.private_data = original.private_data;
    return copy;
}
