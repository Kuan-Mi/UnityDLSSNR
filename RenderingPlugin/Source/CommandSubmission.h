#pragma once

#include <cstdint>

namespace unityrhi
{
struct RhiResource;
struct UploadTicket;

// Native-owned copy of one submitted command stream. Unity's render event
// carries this pointer; the render thread consumes and destroys it after the
// stream has been replayed.
struct CommandSubmission;

CommandSubmission* CreateCommandSubmission(const void* stream, uint32_t byteSize,
    RhiResource* const* resources, uint32_t resourceCount,
    UploadTicket* const* uploadTickets, uint32_t uploadTicketCount);
const void* GetCommandSubmissionStream(CommandSubmission* submission);
void DestroyCommandSubmission(CommandSubmission* submission);
void DestroyAllCommandSubmissions();
} // namespace unityrhi
