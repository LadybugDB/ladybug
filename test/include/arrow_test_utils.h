#pragma once

#include <cstring>
#include <functional>
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
inline void createSchema<float>(ArrowSchema* schema, const char* name) {
    schema->format = "f"; // float
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

// Helper to create a float array from vector
inline void createFloatArray(ArrowArray* array, const std::vector<float>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr; // No nulls
    private_data->data = malloc(data.size() * sizeof(float));
    memcpy(private_data->data, data.data(), data.size() * sizeof(float));

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

template<>
inline void createSchema<uint64_t>(ArrowSchema* schema, const char* name) {
    schema->format = "L"; // uint64 (capital L)
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

// Helper to create a uint64 array from vector
inline void createUint64Array(ArrowArray* array, const std::vector<uint64_t>& data) {
    struct ArrayPrivateData {
        void* validity = nullptr;
        void* data = nullptr;
        int32_t* offsets = nullptr;
    };

    auto* private_data = new ArrayPrivateData();
    private_data->validity = nullptr;
    private_data->data = malloc(data.size() * sizeof(uint64_t));
    memcpy(private_data->data, data.data(), data.size() * sizeof(uint64_t));

    array->length = static_cast<int64_t>(data.size());
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2;
    array->n_children = 0;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr;
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
// Date schema helper (Arrow date32: format "tdD", stores int32 days since 1970-01-01)
inline void createDateSchema(ArrowSchema* schema, const char* name) {
    schema->format = "tdD";
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 0;
    schema->children = nullptr;
    schema->dictionary = nullptr;
    schema->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->private_data = nullptr;
}

// Date array: Arrow date32 values are stored as int32 (days since 1970-01-01)
inline void createDateArray(ArrowArray* array, const std::vector<int32_t>& days) {
    createInt32Array(array, days);
}

// LIST<INT64> schema helper
inline void createListInt64Schema(ArrowSchema* schema, const char* name) {
    schema->format = "+l";
    schema->name = name;
    schema->metadata = nullptr;
    schema->flags = ARROW_FLAG_NULLABLE;
    schema->n_children = 1;
    schema->children = static_cast<ArrowSchema**>(malloc(sizeof(ArrowSchema*)));
    schema->children[0] = static_cast<ArrowSchema*>(malloc(sizeof(ArrowSchema)));
    schema->children[0]->format = "l";
    schema->children[0]->name = "item";
    schema->children[0]->metadata = nullptr;
    schema->children[0]->flags = ARROW_FLAG_NULLABLE;
    schema->children[0]->n_children = 0;
    schema->children[0]->children = nullptr;
    schema->children[0]->dictionary = nullptr;
    schema->children[0]->release = [](ArrowSchema* s) { s->release = nullptr; };
    schema->children[0]->private_data = nullptr;
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

// LIST<INT64> array helper
inline void createListInt64Array(ArrowArray* array,
    const std::vector<std::vector<int64_t>>& lists) {
    int32_t total = 0;
    for (const auto& lst : lists) {
        total += static_cast<int32_t>(lst.size());
    }

    auto* offsets = static_cast<int32_t*>(malloc((lists.size() + 1) * sizeof(int32_t)));
    offsets[0] = 0;
    for (size_t i = 0; i < lists.size(); ++i) {
        offsets[i + 1] = offsets[i] + static_cast<int32_t>(lists[i].size());
    }

    auto* values = static_cast<int64_t*>(malloc(total > 0 ? total * sizeof(int64_t) : 1));
    int32_t pos = 0;
    for (const auto& lst : lists) {
        for (auto value : lst) {
            values[pos++] = value;
        }
    }

    struct ChildPD {
        int64_t* values;
    };
    auto* childPrivateData = new ChildPD{values};
    auto* child = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
    child->length = total;
    child->null_count = 0;
    child->offset = 0;
    child->n_buffers = 2;
    child->n_children = 0;
    child->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    child->buffers[0] = nullptr;
    child->buffers[1] = values;
    child->children = nullptr;
    child->dictionary = nullptr;
    child->release = [](ArrowArray* a) {
        if (a->private_data) {
            auto* privateData = static_cast<ChildPD*>(a->private_data);
            free(privateData->values);
            delete privateData;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        a->release = nullptr;
    };
    child->private_data = childPrivateData;

    struct ListPD {
        int32_t* offsets;
    };
    auto* listPrivateData = new ListPD{offsets};
    array->length = static_cast<int64_t>(lists.size());
    array->null_count = 0;
    array->offset = 0;
    array->n_buffers = 2;
    array->n_children = 1;
    array->buffers = static_cast<const void**>(malloc(sizeof(void*) * 2));
    array->buffers[0] = nullptr;
    array->buffers[1] = offsets;
    array->children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*)));
    array->children[0] = child;
    array->dictionary = nullptr;
    array->release = [](ArrowArray* a) {
        if (a->children) {
            for (int64_t i = 0; i < a->n_children; ++i) {
                if (a->children[i]->release) {
                    a->children[i]->release(a->children[i]);
                }
                free(a->children[i]);
            }
            free(a->children);
        }
        if (a->private_data) {
            auto* privateData = static_cast<ListPD*>(a->private_data);
            free(privateData->offsets);
            delete privateData;
        }
        if (a->buffers) {
            free(const_cast<void**>(a->buffers));
        }
        a->release = nullptr;
    };
    array->private_data = listPrivateData;
}

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

// Build a struct ArrowArray whose children are filled by the given builders.
inline ArrowArrayWrapper createStructArray(int64_t length,
    const std::vector<std::function<void(ArrowArray*)>>& childBuilders) {
    ArrowArrayWrapper array;
    array.length = length;
    array.null_count = 0;
    array.offset = 0;
    array.n_buffers = 1;
    array.n_children = static_cast<int64_t>(childBuilders.size());
    array.buffers = static_cast<const void**>(malloc(sizeof(void*)));
    array.buffers[0] = nullptr;
    array.children = static_cast<ArrowArray**>(malloc(sizeof(ArrowArray*) * childBuilders.size()));
    for (size_t i = 0; i < childBuilders.size(); ++i) {
        array.children[i] = static_cast<ArrowArray*>(malloc(sizeof(ArrowArray)));
        childBuilders[i](array.children[i]);
    }
    array.dictionary = nullptr;
    array.release = [](ArrowArray* arr) {
        if (arr->children) {
            for (int64_t i = 0; i < arr->n_children; ++i) {
                if (arr->children[i]->release) {
                    arr->children[i]->release(arr->children[i]);
                }
                free(arr->children[i]);
            }
            free(arr->children);
        }
        if (arr->buffers) {
            free(const_cast<void**>(arr->buffers));
        }
        arr->release = nullptr;
    };
    array.private_data = nullptr;
    return array;
}
