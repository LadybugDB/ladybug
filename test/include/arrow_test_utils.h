#pragma once

#include <cstring>
#include <memory>
#include <vector>

#include "common/arrow/arrow.h"

// Template helpers to create Arrow schemas for different types
template<typename T>
void createSchema(ArrowSchema* schema, const char* name);

template<>
inline void createSchema<int32_t>(ArrowSchema* schema, const char* name) {
    schema->format = "i"; // int32
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

template<>
inline void createSchema<std::string>(ArrowSchema* schema, const char* name) {
    schema->format = "u"; // utf8 string
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

template<>
inline void createSchema<double>(ArrowSchema* schema, const char* name) {
    schema->format = "g"; // double
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

template<>
inline void createSchema<bool>(ArrowSchema* schema, const char* name) {
    schema->format = "b"; // boolean
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

template<>
inline void createSchema<int64_t>(ArrowSchema* schema, const char* name) {
    schema->format = "l"; // int64
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

// Helper to create a struct schema with multiple fields
inline void createStructSchema(ArrowSchema* schema, int n_fields) {
    schema->format = "+s"; // struct
    schema->name = nullptr;
    schema->metadata = nullptr;
    schema->flags = 0;
    schema->n_children = n_fields;
    schema->children = static_cast<ArrowSchema**>(malloc(sizeof(ArrowSchema*) * n_fields));
    for (int i = 0; i < n_fields; i++) {
        schema->children[i] = static_cast<ArrowSchema*>(malloc(sizeof(ArrowSchema)));
    }
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) {
        if (s->children) {
            for (int64_t i = 0; i < s->n_children; i++) {
                if (s->children[i]->release) {
                    s->children[i]->release(s->children[i]);
                }
                free(s->children[i]);
            }
            free(s->children);
        }
        s->release = nullptr;
    };
    schema->private_data = nullptr;
}

// Helper to create an int32 array from vector
inline void createInt32Array(ArrowArray* array, const std::vector<int32_t>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls
    private_data->data = malloc(data.size() * sizeof(int32_t));
    memcpy(private_data->data, data.data(), data.size() * sizeof(int32_t));

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2; // validity and data
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr; // validity buffer (no nulls)
    array->buffers[1] = private_data->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->validity);
            free(pd->data);
            free(pd->offsets);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; i++) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        a->release = nullptr;
    };
    array->private_data = private_data;
}

// Helper to create an int64 array from vector
inline void createInt64Array(ArrowArray* array, const std::vector<int64_t>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls
    private_data->data = malloc(data.size() * sizeof(int64_t));
    memcpy(private_data->data, data.data(), data.size() * sizeof(int64_t));

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2; // validity and data
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr; // validity buffer (no nulls)
    array->buffers[1] = private_data->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->validity);
            free(pd->data);
            free(pd->offsets);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; i++) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        a->release = nullptr;
    };
    array->private_data = private_data;
}

// Helper to create a string array from vector
inline void createStringArray(ArrowArray* array, const std::vector<std::string>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls

    // Calculate total string length
    int32_t total_length = 0;
    for (const auto& str : data) {
        total_length += str.length();
    }

    // Create offsets buffer (n+1 offsets for n strings)
    private_data->offsets = static_cast<int32_t*>(malloc((data.size() + 1) * sizeof(int32_t)));
    private_data->offsets[0] = 0;
    for (size_t i = 0; i < data.size(); i++) {
        private_data->offsets[i + 1] = private_data->offsets[i] + data[i].length();
    }

    // Create data buffer
    private_data->data = malloc(total_length);
    char* data_ptr = static_cast<char*>(private_data->data);
    for (const auto& str : data) {
        memcpy(data_ptr, str.data(), str.length());
        data_ptr += str.length();
    }

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 3; // validity, offsets, and data
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 3));
    array->buffers[0] = nullptr; // validity buffer (no nulls)
    array->buffers[1] = private_data->offsets;
    array->buffers[2] = private_data->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->validity);
            free(pd->data);
            free(pd->offsets);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; i++) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        a->release = nullptr;
    };
    array->private_data = private_data;
}

// Helper to create a double array from vector
inline void createDoubleArray(ArrowArray* array, const std::vector<double>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls
    private_data->data = malloc(data.size() * sizeof(double));
    memcpy(private_data->data, data.data(), data.size() * sizeof(double));

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2; // validity and data
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr; // validity buffer (no nulls)
    array->buffers[1] = private_data->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->validity);
            free(pd->data);
            free(pd->offsets);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; i++) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        a->release = nullptr;
    };
    array->private_data = private_data;
}

// Helper to create a bool array from vector
inline void createBoolArray(ArrowArray* array, const std::vector<bool>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls

    // Bool arrays are bit-packed, 8 bools per byte
    size_t byte_count = (data.size() + 7) / 8;
    private_data->data = malloc(byte_count);
    memset(private_data->data, 0, byte_count);

    uint8_t* byte_data = static_cast<uint8_t*>(private_data->data);
    for (size_t i = 0; i < data.size(); i++) {
        if (data[i]) {
            byte_data[i / 8] |= (1 << (i % 8));
        }
    }

    array->length = data.size();
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2; // validity and data
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr; // validity buffer (no nulls)
    array->buffers[1] = private_data->data;
    array->children = nullptr;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* pd = static_cast<ArrayPrivateData*>(a->private_data);
            free(pd->validity);
            free(pd->data);
            free(pd->offsets);
            delete pd;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; i++) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        a->release = nullptr;
    };
    array->private_data = private_data;
}
