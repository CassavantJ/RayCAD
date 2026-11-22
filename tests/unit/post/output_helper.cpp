// The child process of the output kill tests (output_test.cpp, task T-047): writes a program through
// writeProgramFile and stops for good at one step, so the test can kill it there like a crash or a pulled plug.
//
//   ray_post_output_helper <write|rename> <folder> <file name> <marker file> <bytes>
//     write   stops after the first chunk of the temporary file is written
//     rename  stops just before the temporary file is renamed into place
// The marker file is created when it stops; then it sleeps until it is killed.
#include "output.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <memory>
#include <string>
#include <thread>

namespace {

using namespace ray;
using namespace ray::post;

[[noreturn]] void stopHere(const std::string& marker)
{
    std::ofstream(marker, std::ios::binary) << "stopped";
    for (;;)
        std::this_thread::sleep_for(std::chrono::seconds(1));
}

class StoppingFile final : public IOutputFile {
public:
    StoppingFile(std::unique_ptr<IOutputFile> inner, std::string marker) : inner_(std::move(inner)), marker_(marker) {}
    Status write(std::string_view bytes) override
    {
        auto ok = inner_->write(bytes);
        if (ok)
            (void)inner_->flush(); // the bytes so far really are in the file when it is killed
        stopHere(marker_);
    }
    Status flush() override { return inner_->flush(); }
    Status close() override { return inner_->close(); }

private:
    std::unique_ptr<IOutputFile> inner_;
    std::string marker_;
};

class StoppingFs final : public IOutputFs {
public:
    StoppingFs(bool inWrite, std::string marker) : inWrite_(inWrite), marker_(std::move(marker)) {}
    Status checkFolder(const std::string& dir) override { return localOutputFs().checkFolder(dir); }
    Result<bool> exists(const std::string& path) override { return localOutputFs().exists(path); }
    Result<std::unique_ptr<IOutputFile>> createNew(const std::string& path) override
    {
        auto f = localOutputFs().createNew(path);
        if (!f || !inWrite_)
            return f;
        return std::unique_ptr<IOutputFile>(std::make_unique<StoppingFile>(std::move(*f), marker_));
    }
    Status replace(const std::string& from, const std::string& to) override
    {
        if (!inWrite_)
            stopHere(marker_);
        return localOutputFs().replace(from, to);
    }
    Status moveNoReplace(const std::string& from, const std::string& to) override
    {
        if (!inWrite_)
            stopHere(marker_);
        return localOutputFs().moveNoReplace(from, to);
    }
    Status remove(const std::string& path) override { return localOutputFs().remove(path); }

private:
    bool inWrite_;
    std::string marker_;
};

} // namespace

int main(int argc, char** argv)
{
    if (argc != 6) {
        std::fprintf(stderr, "usage: ray_post_output_helper <write|rename> <folder> <name> <marker> <bytes>\n");
        return 2;
    }
    const std::string mode = argv[1];
    const std::size_t size = static_cast<std::size_t>(std::stoull(argv[5]));
    std::string text;
    text.reserve(size);
    for (std::size_t line = 0; text.size() < size; ++line)
        text += "G1 X" + std::to_string(line % 1000) + ".5 Y-2.25 F800.\r\n";
    StoppingFs fs(mode == "write", argv[4]);
    OutputTarget target;
    target.directory = argv[2];
    target.fileName = argv[3];
    target.mode = OverwriteMode::Replace;
    auto written = writeProgramFile(text, target, fs);
    std::fprintf(stderr, "the helper was not stopped: %s\n", written ? "written" : written.error().message.c_str());
    return 3;
}
