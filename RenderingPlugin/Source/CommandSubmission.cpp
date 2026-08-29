#include "CommandSubmission.h"

#include <array>
#include <cstring>
#include <mutex>
#include <new>
#include <unordered_set>
#include <vector>

#include "CommandStream.h"
#include "d3d12/d3d12-backend.h"

namespace unityrhi
{
struct CommandSubmission
{
    std::vector<uint8_t> stream;
    std::vector<RhiResource*> resources;
    std::vector<UploadTicket*> uploadTickets;
};

namespace
{
std::mutex g_SubmissionMutex;
std::unordered_set<CommandSubmission*> g_Submissions;
constexpr size_t kMaxPooledSubmissions = 32;
std::array<CommandSubmission*, kMaxPooledSubmissions> g_SubmissionPool{};
size_t g_SubmissionPoolSize = 0;
}

CommandSubmission* CreateCommandSubmission(const void* stream, uint32_t byteSize,
    RhiResource* const* resources, uint32_t resourceCount,
    UploadTicket* const* uploadTickets, uint32_t uploadTicketCount)
{
    if (!stream || byteSize < sizeof(CommandStreamHeader) ||
        (resourceCount != 0 && !resources) || (uploadTicketCount != 0 && !uploadTickets))
        return nullptr;

    Device* device = Device::Get();
    if (!device)
        return nullptr;

    CommandSubmission* submission = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_SubmissionPoolSize != 0)
        {
            submission = g_SubmissionPool[--g_SubmissionPoolSize];
            g_SubmissionPool[g_SubmissionPoolSize] = nullptr;
        }
    }
    if (!submission)
        submission = new (std::nothrow) CommandSubmission{};
    if (!submission)
        return nullptr;
    submission->stream.clear();
    submission->resources.clear();
    submission->uploadTickets.clear();

    try
    {
        submission->stream.resize(byteSize);
        std::memcpy(submission->stream.data(), stream, byteSize);
        if (resourceCount != 0)
            submission->resources.assign(resources, resources + resourceCount);
        if (uploadTicketCount != 0)
            submission->uploadTickets.assign(uploadTickets, uploadTickets + uploadTicketCount);
    }
    catch (...)
    {
        submission->stream.clear();
        submission->resources.clear();
        submission->uploadTickets.clear();
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_SubmissionPoolSize < kMaxPooledSubmissions)
            g_SubmissionPool[g_SubmissionPoolSize++] = submission;
        else
            delete submission;
        return nullptr;
    }

    if (!device->RetainSubmissionResources(submission->resources.data(), resourceCount))
    {
        submission->stream.clear();
        submission->resources.clear();
        submission->uploadTickets.clear();
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_SubmissionPoolSize < kMaxPooledSubmissions)
            g_SubmissionPool[g_SubmissionPoolSize++] = submission;
        else
            delete submission;
        return nullptr;
    }

    for (UploadTicket* ticket : submission->uploadTickets)
        device->uploadManager().retainTicket(ticket);

    try
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        g_Submissions.insert(submission);
    }
    catch (...)
    {
        device->ReleaseSubmissionResources(submission->resources.data(), resourceCount);
        for (UploadTicket* ticket : submission->uploadTickets)
            device->uploadManager().releaseTicket(ticket);
        submission->stream.clear();
        submission->resources.clear();
        submission->uploadTickets.clear();
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_SubmissionPoolSize < kMaxPooledSubmissions)
            g_SubmissionPool[g_SubmissionPoolSize++] = submission;
        else
            delete submission;
        return nullptr;
    }
    return submission;
}

const void* GetCommandSubmissionStream(CommandSubmission* submission)
{
    return submission && !submission->stream.empty() ? submission->stream.data() : nullptr;
}

void DestroyCommandSubmission(CommandSubmission* submission)
{
    if (!submission)
        return;
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_Submissions.erase(submission) == 0)
            return;
    }

    if (Device* device = Device::Get())
    {
        device->ReleaseSubmissionResources(submission->resources.data(),
            uint32_t(submission->resources.size()));
        for (UploadTicket* ticket : submission->uploadTickets)
            device->uploadManager().releaseTicket(ticket);
    }
    submission->stream.clear();
    submission->resources.clear();
    submission->uploadTickets.clear();
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        if (g_SubmissionPoolSize < kMaxPooledSubmissions)
        {
            g_SubmissionPool[g_SubmissionPoolSize++] = submission;
            return;
        }
    }
    delete submission;
}

void DestroyAllCommandSubmissions()
{
    std::vector<CommandSubmission*> submissions;
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        submissions.assign(g_Submissions.begin(), g_Submissions.end());
    }
    for (CommandSubmission* submission : submissions)
        DestroyCommandSubmission(submission);
    std::array<CommandSubmission*, kMaxPooledSubmissions> pool{};
    size_t poolSize = 0;
    {
        std::lock_guard<std::mutex> lock(g_SubmissionMutex);
        pool = g_SubmissionPool;
        poolSize = g_SubmissionPoolSize;
        g_SubmissionPool.fill(nullptr);
        g_SubmissionPoolSize = 0;
    }
    for (size_t i = 0; i < poolSize; ++i)
        delete pool[i];
}
} // namespace unityrhi
