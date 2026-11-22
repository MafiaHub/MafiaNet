# MafiaNet implementation plans

| Plan | Baseline | Priority | Status |
|---|---|---|---|
| [001 · Built-in object replication and spatial streaming](001-object-replication-and-streaming.md) | `2ba07f5d` | P1 | Proposed |

Every UDP peer owns its replication service and exposes it through `GetReplication()`. The plan contains seven independently reviewable stages. The lifetime work comes first. The spatial policy can be developed against a reference model while the public manager is built. The three-peer acceptance scenario joins the work before release.
