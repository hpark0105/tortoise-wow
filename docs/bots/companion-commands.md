# Companion bot commands

In-game chat commands for persistent owned companions. All are
`SEC_PLAYER`, work in-world only, and for owned companions are owner-only:
the issuer must be the account bound in `bot_ownership`. Rejections are
printed in chat (`Bot ... rejected.`) and logged server-side
(`docker logs tortoise-local-world-1`) with the reason.

## Commands

| Command | What it does |
| --- | --- |
| `.botrecruit <botname>` | Invites the bot into your party. The bot session is socketless and never answers invites itself, so the server settles them: the owner's invite is accepted, anyone else's is declined. Rejected if the bot is offline (benched), the party is full, or you are not the owner. Party leadership does not matter: if the bot already holds the party (it becomes leader when you log out, see notes), re-recruiting works, and recruiting a bot that is already in your party is a no-op. |
| `.botdismiss <botname>` | Removes the bot from your party and clears its follow, hold, assist, and defend orders. It stays online and wanders locally around its release point without auto-pulling enemies. Party leadership does not matter. A 2-person party disbands on either path (classic rule). |
| `.botrecall <botname>` | Logs the bot in. After a world restart, owned companions stay benched (offline) until recalled; recall queues a login for the bot character. If the bot came back into a stale group persisted from before a restart (one you are no longer in), it leaves that group first so it can join yours. |
| `.botfollow <botname>` | Orders the bot to follow you. It pathfinds to you (same map) and stays within ~2 yards. A new order invalidates the previous one by sequence number, so a stale follow can never re-arm. |
| `.botstop <botname>` | Cancels the follow order. With no other active order, an owned companion returns to its default: it holds position near you and pathfinds to catch up if you move more than 25 yd away on the same map (ambient bots resume the legacy idle wander and auto-hunt). |
| `.bothold <botname>` | (alias `.bothyld`) Freezes the bot in place: stops movement and offense, clears follow. Highest priority -- a delayed older follow/assist cannot cancel a hold. Any new order releases it. |
| `.botassist <botname> <targetname>` | Orders the bot to fight the named creature (target name may contain spaces; it is the rest of the line). The bot must be in your party and the target a legal hostile creature. If the target dies, despawns, or unloads, the assist clears and the bot resumes its previous order. |
| `.botdefend <botname> on\|off` | Toggles reactive defend. While enabled, a following bot engages a creature that is actively attacking you or the bot, and stands down (`cleared reason:owner safe`) when no attacker remains. It never pulls bystanders. Session-scoped: a world restart clears it. |
| `.botinit [botname]` | Without a name, sets up owned companions only until the normal party has no free slots; additional roster members are skipped, never queued in a burst. With a name, sets up only that companion: recalls it (logging it in if needed), puts it in your party, enables reactive defend, and arms follow. Safe to re-run. Reports `N set, N queued, N skipped`. |

## Order priority

When several orders are live, selection is deterministic:

    Hold  >  Assist  >  ContinueCombat  >  Defend  >  Follow

- `ContinueCombat`: the bot is already fighting (something hit it, or an
  active order targets a live creature). `.botfollow` issued mid-fight does
  not yank it out; it finishes the fight and then follows.
- `Defend` only fires when the selection would otherwise be Follow (or
  Loot) and a legal attacker is in range.
- With **no** active order at all, an owned companion in your party runs
  default owner-follow: hold position, and pathfind to catch up if the owner
  is more than 25 yd away on the same map. An ungrouped owned companion
  wanders around its release point, but never auto-engages (self-defense and
  explicit orders only). Ambient bots (no owner) run the legacy loop: wander
  locally (8-20 yd), auto-engage the nearest hostile within 30 yd, loot the
  corpse, repeat.

## Behavior notes

- Offline/benched: after any world restart, owned companions are offline and
  `.botrecruit` is rejected until you `.botrecall` them.
- Death and recovery: if the bot dies while it has an active order
  (follow/hold/assist), it waits out the normal corpse-reclaim delay, walks
  to its corpse if needed, and resurrects in place at 50% HP (no teleport,
  no second mechanism). If it dies with **no** active order (default
  owner-follow, or legacy auto-hunt for an ambient bot), it stays
  dead-idle at the corpse until you give it an order or recall it.
- Leadership drift: classic rules transfer party leadership to the bot
  when the owner logs out while the bot is in the party. The bot keeps
  leading until the owner reclaims it (party frame) or the party disbands;
  `.botrecruit` and `.botdismiss` work regardless of who leads.
- Party requirement: `.botassist` requires the bot to be in your party;
  follow/hold/defend do not (ownership is enough).
- Legacy admin: `.discbot stop` (`SEC_ADMINISTRATOR`) belongs to the
  Discord-bot integration build flag, not the companion system.

## Typical session flow

    .botinit                      # one-shot: recall + party + defend + follow (all owned companions)
    .botinit Companion            # same setup for just one companion
    .botrecall Companion          # (manual) bring it online (after restart)
    .botrecruit Companion         # party up
    .botfollow Companion          # it walks to you and follows
    .botdefend Companion on       # fight back when something hits you
    .botassist Companion <mob>    # fight this one
    .bothold Companion            # stop, stand still
    .botstop Companion            # cancel follow; it stays near you and catches up
    .botdismiss Companion         # release from party; wanders locally, stays online
