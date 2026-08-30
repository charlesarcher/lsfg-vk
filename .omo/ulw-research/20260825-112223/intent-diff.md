# intent-diff
| id | expected truth | observed | status |
|---|---|---|---|
| I1 | conf.toml with gpu="<name>" activates dual-GPU without env vars | UNTESTED | unknown |
| I2 | wrong gpu name fails with named error quoting the string | seen via ENV path; conf path untested | unknown |
| I3 | unset gpu = game's own device | verified (ENV + CLI) | true |
| I4 | vkcube Intel+FG=AMD works via layer | verified (task-20/21) | true |
| I5 | all 9 GPU combos work | verified (task-21) | true |
