"""BL-008 pure owner-control authorization/state contract."""
COMMANDS = {"start", "status", "pause", "resume", "explain", "rollback"}
def authorize(command, owner, target_owner, mode="observe"):
    if not owner or owner != target_owner or command not in COMMANDS:
        return {"ok": False, "mode": mode, "reason": "unauthorized"}
    if command == "start": mode = "observe"
    elif command == "pause": mode = "paused"
    elif command == "resume" and mode == "paused": mode = "observe"
    elif command == "rollback": mode = "observe"
    return {"ok": True, "mode": mode, "command": command}
