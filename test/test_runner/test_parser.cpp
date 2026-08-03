void TestParser::openFile() {
    if (access(path.c_str(), 0) != 0) {
        throw TestException("Test file not exists [" + path + "].");
    }
    // Open in binary mode so tellg/seekg offsets match byte positions on all
    // platforms. In MSVC text mode, getline over LF-only files advances the
    // stream position one extra byte per line, which makes
    // setCursorToPreviousLine() re-read from a wrong offset (Windows-only
    // test runner bug). CRLF is handled in nextLine() by stripping '\r'.
    fileStream.open(path, std::ios::binary);
}
