# MCP - Model Context Protocol Tools

| Tool | Description (short) | Parameters |
|------|----------------------|------------|
| engine_info | Engine state: scenes count, names. | none |
| world_list | List worlds with state, tick stats. | worlds array |
| world_tree | Tree of instances depth‑limited & count‑limited. | depth, limit |
| instance_get | Get properties of one entity in a world. | id (int), optional component string |
| instance_set | Set one property on an entity. | id (int) component string value |
| engine_components | Engine‑wide component catalogue with sizes and flags. | none |
| component_list | List components declared by the game + fields. | none |
| entity_query | Query all entities carrying given components. | components array, limit int |
| component_get | Read fields of one component on an entity. | id (int) component string |
| component_set | Write fields of a component to an entity. | id (int) component string fields object |
| profile_frame | Flame‑graph frame info with limited spans. | limit int |
| layer_table | Engine’s layer stack bottom‑to‑top, modules per layer. | none |
| module_get | Get description of one module or program. | name string |
| module_may_link | Is a link allowed between two modules? | from (string) to (string) |
| class_list | All registered classes + count. | none |
| class_get | Properties of one class, optional inheritance check. | derivedFrom string |
| script_check | Type‑check Luau source or file. | source path |
| log_tail | Get recent log lines filtered by level/category. | level (enum), category (string) lines int |
| log_level | Read or change logging configuration. | set (category=level terms) |
| metrics_read | Read all counters, gauges and histograms optionally prefixed. | prefix string |
| test_run | Start test suites non‑blocking; returns handle. | all bool, verbose bool |
| test_result | Get status/result of a `test_run`. | none |
