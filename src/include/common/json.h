#pragma once

#include <memory>
#include <string>
#include <vector>

#include "yyjson.h"

namespace lbug {

class JsonValue;
class JsonMutValue;
class JsonMutDoc;

class JsonValue {
public:
    JsonValue() : val_{nullptr} {}
    explicit JsonValue(yyjson_val* val) : val_{val} {}

    const char* getStr() const { return yyjson_get_str(val_); }
    double getReal() const { return yyjson_get_real(val_); }
    int64_t getInt() const { return yyjson_get_int(val_); }
    uint64_t getUint() const { return yyjson_get_uint(val_); }
    bool getBool() const { return yyjson_get_bool(val_); }

    bool isObj() const { return yyjson_get_type(val_) == YYJSON_TYPE_OBJ; }
    bool isArr() const { return yyjson_get_type(val_) == YYJSON_TYPE_ARR; }
    bool isStr() const { return yyjson_get_type(val_) == YYJSON_TYPE_STR; }
    bool isReal() const { return yyjson_get_type(val_) == YYJSON_TYPE_NUM; }
    bool isInt() const { return yyjson_get_type(val_) == YYJSON_TYPE_NUM; }
    bool isNull() const { return yyjson_get_type(val_) == YYJSON_TYPE_NULL; }

    JsonValue getObjKey(const char* key) const { return JsonValue(yyjson_obj_get(val_, key)); }

    JsonValue getArr(size_t idx) const { return JsonValue(yyjson_arr_get(val_, idx)); }

    size_t getArrSize() const { return yyjson_arr_size(val_); }

    yyjson_val* val_;
};

class JsonMutValue {
public:
    JsonMutValue() : val_{nullptr} {}
    explicit JsonMutValue(yyjson_mut_val* val) : val_{val} {}

    void addStr(yyjson_mut_doc* doc, const char* key, const char* val) {
        yyjson_mut_obj_add_str(doc, val_, key, val);
    }
    void addSint(yyjson_mut_doc* doc, const char* key, int64_t val) {
        yyjson_mut_obj_add_sint(doc, val_, key, val);
    }
    void addUint(yyjson_mut_doc* doc, const char* key, uint64_t val) {
        yyjson_mut_obj_add_uint(doc, val_, key, val);
    }
    void addReal(yyjson_mut_doc* doc, const char* key, double val) {
        yyjson_mut_obj_add_real(doc, val_, key, val);
    }
    void addBool(yyjson_mut_doc* doc, const char* key, bool val) {
        yyjson_mut_obj_add_bool(doc, val_, key, val);
    }
    void addNull(yyjson_mut_doc* doc, const char* key) { yyjson_mut_obj_add_null(doc, val_, key); }
    void addVal(yyjson_mut_doc* doc, const char* key, yyjson_mut_val* childVal) {
        yyjson_mut_obj_add(val_, yyjson_mut_strcpy(doc, key), childVal);
    }

    JsonMutValue addObj(yyjson_mut_doc* doc, const char* key) {
        return JsonMutValue(yyjson_mut_obj_add_obj(doc, val_, key));
    }
    JsonMutValue addArr(yyjson_mut_doc* doc, const char*) {
        return JsonMutValue(yyjson_mut_arr_add_obj(doc, val_));
    }
    JsonMutValue addArrStr(yyjson_mut_doc* doc, const char*) {
        return JsonMutValue(yyjson_mut_arr_add_arr(doc, val_));
    }

    yyjson_mut_val* val_;
};

class JsonMutDoc {
public:
    JsonMutDoc() : doc_{yyjson_mut_doc_new(nullptr)} {}
    ~JsonMutDoc() { yyjson_mut_doc_free(doc_); }

    JsonMutDoc(const JsonMutDoc&) = delete;
    JsonMutDoc& operator=(const JsonMutDoc&) = delete;
    JsonMutDoc(JsonMutDoc&& other) noexcept : doc_{other.doc_} { other.doc_ = nullptr; }
    JsonMutDoc& operator=(JsonMutDoc&& other) noexcept {
        if (this != &other) {
            yyjson_mut_doc_free(doc_);
            doc_ = other.doc_;
            other.doc_ = nullptr;
        }
        return *this;
    }

    JsonMutValue addRoot() {
        auto root = yyjson_mut_obj(doc_);
        yyjson_mut_doc_set_root(doc_, root);
        return JsonMutValue(root);
    }

    std::string toString() const {
        char* str = yyjson_mut_write(doc_, 0, nullptr);
        std::string result(str);
        free(str);
        return result;
    }

    yyjson_mut_doc* doc_;
};

class JsonDoc {
public:
    JsonDoc() : doc_{nullptr}, buffer_{nullptr} {}
    JsonDoc(yyjson_doc* doc, std::shared_ptr<char[]> buffer = nullptr)
        : doc_{doc}, buffer_{std::move(buffer)} {}
    ~JsonDoc() { yyjson_doc_free(doc_); }

    JsonDoc(JsonDoc&& other) noexcept {
        doc_ = other.doc_;
        buffer_ = std::move(other.buffer_);
        other.doc_ = nullptr;
    }

    JsonValue getRoot() { return JsonValue(yyjson_doc_get_root(doc_)); }

    yyjson_doc* doc_;
    std::shared_ptr<char[]> buffer_;
};

inline JsonDoc parseJson(const std::string& str) {
    auto doc = yyjson_read(str.c_str(), str.size(), 0);
    return JsonDoc(doc);
}

inline JsonDoc parseJsonNoError(const std::string& str) {
    auto doc = yyjson_read(str.c_str(), str.size(), YYJSON_READ_NOFLAG);
    return JsonDoc(doc);
}

} // namespace lbug
