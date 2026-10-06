#include <universal/q_shared.h>
#include <qcommon/qcommon.h>
#include <universal/com_files.h>
#include <client/client.h>
#include <script/scr_readwrite.h>
#include <server/server.h>

#include <platform/switch/switch_async_file_writer.h>

#include "switch_perf.h"
#include "switch_save_writer.h"

#include <vector>


namespace
{
void MakeDirs(const std::string &path)
{
    char buf[512];
    I_strncpyz(buf, path.c_str(), sizeof(buf));
    FS_CreatePath(buf);
}

AsyncFileWriter &Writer()
{
    static AsyncFileWriter writer(MakeDirs);
    return writer;
}

// The worker reports its own completion; Com_Printf is safe off the main
// thread (the DB and sound threads use it).
void Report(const AsyncFileWriter::Result &r)
{
    if (r.ok)
        Com_Printf(CON_CHANNEL_FILES, "save file '%s' written: %u bytes, %u ms\n", r.finalPath.c_str(),
            (unsigned int)r.bytes, r.ms);
    else
        Com_PrintError(CON_CHANNEL_FILES, "save file '%s' write failed\n", r.finalPath.c_str());
}
} // namespace

static void AppendSink(void *ctx, const void *data, unsigned int len)
{
    std::vector<uint8_t> *out = (std::vector<uint8_t> *)ctx;
    out->insert(out->end(), (const uint8_t *)data, (const uint8_t *)data + len);
}

void SwitchSave_Submit(const SaveHeader *header, const unsigned char *body)
{
    SwitchPerfStage stage("save_enqueue");
    static bool s_reporterSet;
    if (!s_reporterSet)
    {
        Writer().SetReporter(Report);
        s_reporterSet = true;
    }

    const size_t headerSize = sizeof(SaveHeader);
    AsyncFileWriter::Job job;
    job.data = Writer().AcquireBuffer();
    job.data.reserve(headerSize + (size_t)header->bodySize);
    job.data.insert(job.data.end(), (const uint8_t *)header, (const uint8_t *)header + headerSize);
    job.data.insert(job.data.end(), body, body + header->bodySize);

    // The same trailing records the synchronous write streams after the body.
    SaveImmediate immediate;
    immediate.f = &job.data;
    immediate.sink = AppendSink;
    Scr_SaveSourceImmediate(&immediate);
    if (header->demoPlayback)
        SV_SaveDemoImmediate(&immediate);

    char ospath[260];
    FS_BuildOSPath(fs_homepath->current.string, fs_gamedir, "save/temp.svg", ospath);
    job.tmpPath = ospath;
    FS_BuildOSPath(fs_homepath->current.string, "players", header->filename, ospath);
    job.finalPath = ospath;
    SwitchSaveThumb_Request(ospath);
    Writer().Submit(std::move(job));
}

void SwitchSave_WaitForWrites(void)
{
    Writer().WaitIdle();
}
