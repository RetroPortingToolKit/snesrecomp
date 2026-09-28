# Declared callee exit widths

Use a declaration only when the routine's actual return modes are established
by its implementation or verified analysis. A wrong width changes how the
caller decodes instruction operands.

`func Callee 8000 exit_mx:1,0` declares the same exit for every entry variant.
It now feeds the same table as `exit_mx_at 008000 1 0` instead of being ignored.
Standalone `exit_mx_at` declarations override inline ones.

`exit_mx_variant 008000 1 0 0 1` declares that the callee at `$00:8000`, entered
with M=1/X=0, returns with M=0/X=1. The four widths must be 0 or 1. This exact
entry declaration overrides a broadcast declaration only for that variant.
Conflicting duplicates are rejected. Native analysis and Python regeneration
consume it; regeneration preserves authored facts when refreshing derived exits.

For a single entry variant that can return in several widths, use the existing
`exit_mx_set 008000 M1X0 M0X0,M1X1` form instead.
