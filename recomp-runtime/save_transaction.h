#ifndef RECOMP_SAVE_TRANSACTION_H
#define RECOMP_SAVE_TRANSACTION_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Set before recomp_save_initialize: the folder below .recomp-storage
   whose partition 1 and journal a netplay session plays from. NULL, the
   default, means the player's own saves. */
void recomp_save_set_session(const char *session);
const char *recomp_save_get_session(void);

/* Recover before guest startup. Protects process interruption, not power loss.
   On Windows, hold exclusive ownership through save-undo-v1/lock until exit
   or reinitialization. The reserved lock file is empty and persists on disk.
   Callers must close UDATA handles before ending an operation or recovering. */
bool recomp_save_initialize(const char *disc_root);
bool recomp_save_begin(uint32_t owner);
/* False means an aborted operation or an error; never continue guest writes
   after an error. A nested failure also makes the outer operation abort. */
bool recomp_save_end(uint32_t owner, bool success);
/* True when this recomp_save_end call would roll the profile back. */
bool recomp_save_end_recovers(uint32_t owner, bool success);
void recomp_save_note_failure(uint32_t owner);
/* A required profile mutation rejected for a foreign owner aborts the save. */
void recomp_save_note_pending_failure(void);
bool recomp_save_active(uint32_t owner);
bool recomp_save_pending(void);

#ifdef __cplusplus
}

#include <filesystem>

/* storage/session/partition1, or storage/partition1 when session is NULL. */
inline std::filesystem::path recomp_save_partition1(const std::filesystem::path &storage, const char *session)
{
    return (session != nullptr ? storage / session : storage) / "partition1";
}
#endif
#endif
