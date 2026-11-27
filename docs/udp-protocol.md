# CircuitCheck Pro UDP protocol

Use this same UDP communication protocol across all three BeagleY-AI boards and the dashboard.

## Ports

| Endpoint | UDP port |
| --- | ---: |
| Board 1 (Controller) | 35101 |
| Board 2 (Measurement) | 35102 |
| Board 3 (Signal Generator) | 35103 |
| Dashboard-to-controller interface | 35100 |

## Commands

| Datagram | Behavior |
| --- | --- |
| `PING,nonce` | Reply with `PONG,board_number,nonce`. |
| `PREPARE,test_id,test_type,fault` | Board 3 prepares the circuit safely. |
| `ARM,test_id,test_type` | Board 2 prepares measurement acquisition. |
| `START,test_id,test_type` | Board 3 begins signal generation. |
| `ACK,test_id,command` | Confirms that the requested operation has completed or is ready. |
| `RESULT,test_id,JSON_payload` | Board 2 returns measurement results. |
| `ABORT,test_id` | Cancels the test and safely stops hardware activity. |
| `ERROR,test_id,reason` | Reports an error. |

Test types are `DIVIDER`, `RC`, and `PWM`.

Fault types are `NONE`, `WRONG_VALUE`, `WRONG_FREQ`, and `NO_SIGNAL` where applicable.

Board 2 classifies faults from the actual measurements. Only Board 3 receives the requested fault in `PREPARE`.

All responses go back to the controller's source IP and port. Duplicate commands with the same test ID must never restart hardware operations. Board 2 retains its result until the controller sends `ACK,test_id,RESULT`.

## Board addresses

Once connected to the same network, run `ip -4 -brief address show scope global` on each BeagleY-AI. Send Person 2 your board number and the IPv4 address for the shared-network interface.

Protocol supplied by the team and formatted with Codex assistance.
