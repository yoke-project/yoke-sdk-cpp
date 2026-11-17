# The QML binding

| | |
| --- | --- |
| **Feature** | the QML binding of the interface library, an optional component of the family: a QML document instantiates an attachment to a channel and reads what the channel sees as properties; every operation returns at once with the number of its call and answers later, as a signal and to the functions the document passed; a refusal reaches the document as an object of its code and its detail; the standing subscription's events, a subscription's deliveries and a stream's frames arrive as signals, in order; an attachment the Core ended is a state the document reads; nothing is sent the document did not ask for; it wraps the interface library and adds no coverage, and nothing of the plugin or interface library links Qt; and a consumer finds it as the component `qml` of the same package |
| **Planning item** | yoke-project/yoke-sdk-cpp#63 |

## yoke-sdk-cpp:the-qml-binding.01 — attaching surfaces the opening as properties

| Field | Value |
| --- | --- |
| **Cites** | specs/90.22 · specs/90.23 · specs/70.9 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose opening carries one unit's record `acquire` at sequence 4, the standing subscription `standing` and version 1 |
| **Action** | load a QML document holding an `Attachment` whose `address` is the Core's socket |
| **Expected** | the status passes from `Attaching` to `Attached`; `version` is 1, `subscription` is `standing`, `picture.at` is 4, and `records` holds one record whose `unit.declared.identity` is `acquire` |

## yoke-sdk-cpp:the-qml-binding.02 — every operation returns at once and answers later

| Field | Value |
| --- | --- |
| **Cites** | specs/90.22 · specs/70.12 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that answers a question to `slow` only after 300 ms, a command to `fast` at once with the outcome `done` and the line `ok`, and a read with one record |
| **Action** | from the document, once attached: ask `slow` with the payload `hello`, command `fast`, and read `unit`, each passing a function for its answer; then command `fast` with a payload that is neither a string nor bytes |
| **Expected** | the three calls together return within 50 ms, each with a number of its own; the command's and the read's answers arrive before the question's; the question's answer is the bytes the Core sent, read as `slow` by `text`; the command's is `OUTCOME_DONE` with the line `ok`, also signalled as `answered` with its number; the last command is refused `binding.payload`, later and not at the call, and nothing was sent for it |

## yoke-sdk-cpp:the-qml-binding.03 — a refusal reaches the document with its code and its detail

| Field | Value |
| --- | --- |
| **Cites** | specs/90.23 · specs/70.27 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that refuses a read with `subject.unknown` naming the unit `nobody`, and a command with `channel.suspended` in grade `read-only` in favour of `service` |
| **Action** | from the document, read `nobody` and command a unit, each passing a function for its refusal |
| **Expected** | the read's refusal carries the code `subject.unknown` and the subject of kind `unit` and identity `nobody`; the command's carries `channel.suspended`, the grade `read-only` and `by` `service`; both are also signalled as `refused` with their numbers |

## yoke-sdk-cpp:the-qml-binding.04 — subscriptions arrive as signals, in order, and only the document confirms

| Field | Value |
| --- | --- |
| **Cites** | specs/70.7 · specs/70.32 · specs/90.32 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose standing subscription carries three events and then an overflow's snapshot at sequence 9, and which answers a subscription narrowed to the kind `unit` with its snapshot and two events |
| **Action** | load a document holding an `Attachment` and a `Subscription` of it narrowed to `unit`; wait; then confirm the subscription at the second event's sequence |
| **Expected** | the attachment signals the three events in order and its `picture.at` becomes 9; the subscription opens, signals its snapshot and then its two events in order, and its `id` is the one the Core answered; the Core was sent the filter's kind; no confirmation was sent until the document confirmed, and then one, of that subscription at that sequence |

## yoke-sdk-cpp:the-qml-binding.05 — a stream's frames arrive as signals, in order, and letting it go releases it

| Field | Value |
| --- | --- |
| **Cites** | specs/70.6 · specs/90.22 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that answers a subscription to the stream `cpu` of `hw` with a delivery `d-1` on the connection, flowing, and sends three frames on it |
| **Action** | load a document holding an `Attachment` and a `Delivery` of `hw`'s `cpu`; then make the delivery inactive |
| **Expected** | the delivery opens with `id` `d-1`, `flowing` true and `arrives` `connection`; the three frames are signalled in order, each with its sequence, its clock and its payload as bytes; once inactive, the Core is sent `stream.unsubscribe` naming `d-1` |

## yoke-sdk-cpp:the-qml-binding.06 — an attachment the Core ended is a state the document reads

| Field | Value |
| --- | --- |
| **Cites** | specs/70.31 · specs/90.23 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that ends the attachment after its first request; and no Core at all at a second address |
| **Action** | attach, and read once; then attach to the second address |
| **Expected** | the first attachment's status becomes `Ended`, `attached` false, and `failure` carries a code; a command issued after it is refused, later, as not attached; the second attachment's status becomes `Ended` with a `failure` naming why, and nothing waits on the document's thread for either |

## yoke-sdk-cpp:the-qml-binding.07 — nothing of the plugin or interface library links Qt

| Field | Value |
| --- | --- |
| **Cites** | specs/90.5 · specs/90.22 |
| **Level** | L1 |
| **Method** | check |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the family's library as built with the binding |
| **Action** | list every symbol the library defines or needs |
| **Expected** | none is Qt's: a family built without the binding is the family as it was, and a program written with either library needs nothing of Qt |

## yoke-sdk-cpp:the-qml-binding.08 — a consumer finds the binding as a component of the same package

| Field | Value |
| --- | --- |
| **Cites** | specs/90.22 |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | the family built with the binding, installed under a prefix of the case's own |
| **Action** | configure a project asking `find_package(yoke-sdk-cpp REQUIRED COMPONENTS qml)` against that prefix; then load, in a program that links only Qt, a document importing `Yoke.Interface` from the import path the package names |
| **Expected** | the package is found with its component `qml` and names an import path under the prefix; the document loads and instantiates an `Attachment` |

## yoke-sdk-cpp:the-qml-binding.09 — an attachment the document asks for again is attached when its channel is back

| Field | Value |
| --- | --- |
| **Cites** | specs/90.48 · arch/90-sdks/06 §An interface may attach again, and may not do it quietly · arch/70-interface-surface/03 §What ends an attachment |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core that ends the attachment at its first request without answering it; a document attaching with `attachAgain` set |
| **Action** | the document commands a unit once attached; the Core goes away and its socket with it, for most of a second, and a second document attaches without `attachAgain`; then a second Core, with another opening, serves the same channel |
| **Expected** | the attachment reads `Ended`, the command in flight is answered as refused, and nothing tries the first Core again on the same socket; with no channel it stays `Ended`, and so does the second document's; once the channel's socket is made again it is attached again, a second time, its picture the second Core's, and that Core is never sent the command; the second document stays `Ended` |

## yoke-sdk-cpp:the-qml-binding.10 — a suspended channel is a property, with the channel that prevails

| Field | Value |
| --- | --- |
| **Cites** | specs/70.6 · arch/70-interface-surface/06 §Arbitration · arch/45-events/06 §The catalogue |
| **Level** | L1 |
| **Method** | test |
| **Not applicable in** | — |
| **Label** | blocking |
| **Precondition** | a Core whose opening picture holds the channel's own record, suspended, dark, by `panel`; and whose standing subscription then carries `channel.resumed` and `channel.suspended`, read-only, by `bench` |
| **Action** | attach, and record each change of the suspension |
| **Expected** | the document reads three changes: dark by `panel`, then active, then read-only by `bench`, each with its reason; it is attached throughout; nothing was asked of the Core to learn them |
