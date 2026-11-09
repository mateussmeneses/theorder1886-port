# Planted failures

Run before trusting any passing oracle, and again after every oracle change.

| # | Mutation | Where | Expected | Result |
|---|---|---|---|---|
| P1 | Change one constant (e.g. threshold 128.0 -> 0.0) | | rejected | |
| P2 | Swap one branch (if/else, comparison direction) | | rejected | |
| P3 | Shift one boundary by one (`<` vs `<=`, length - 1) | | rejected | |

If any row is not rejected: fix the oracle, then re-run every case it accepted before.

Prompt:

```
Add three deliberately wrong variants of the implementation to ./oracle-tests:
change one constant, swap one branch, shift one boundary by one.
Run the oracle on each and confirm all three are rejected.
If any passes, fix the oracle first and list every earlier result it may have wrongly accepted.
```
