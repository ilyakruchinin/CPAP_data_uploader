#ifndef UPLOAD_FSM_H
#define UPLOAD_FSM_H

// Upload FSM states — shared between main.cpp and WebServer
enum class UploadState {
    IDLE,
    LISTENING,
    ACQUIRING,
    UPLOADING,
    RELEASING,
    COOLDOWN,
    COMPLETE,
    MONITORING
};

inline const char* getStateName(UploadState state) {
    switch (state) {
        case UploadState::IDLE: return "IDLE";
        case UploadState::LISTENING: return "LISTENING";
        case UploadState::ACQUIRING: return "ACQUIRING";
        case UploadState::UPLOADING: return "UPLOADING";
        case UploadState::RELEASING: return "RELEASING";
        case UploadState::COOLDOWN: return "COOLDOWN";
        case UploadState::COMPLETE: return "COMPLETE";
        case UploadState::MONITORING: return "MONITORING";
        default: return "UNKNOWN";
    }
}

#endif // UPLOAD_FSM_H
