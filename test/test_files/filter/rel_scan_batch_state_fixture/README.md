# Nested relationship-scan filtering fixture

This generic graph has 1,228 nodes, 114 `Link` edges and 23 `Member` edges. Nodes have only a numeric identity, a string key, a matching value and a category. Relationships have only a label. The CSV files are the complete input; no external dataset, extension or application schema is required.

The query joins left and right parent nodes through `Link`, then matches their member nodes by value. It must exclude a link labelled `other` when the predicate requires `match`. A positive case inserts a matching link and requires only that link in the output.

Row order and the node/edge distribution retain the multiple batches and nested expansion that expose stale chunk factorization state. A small two-hop graph did not reproduce it; global minimality is not claimed.
