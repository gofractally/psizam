;; Compiler correctness fixture: there is no defined function to own the unit.
(module (import "env" "host" (func (param i64 i32) (result i64))))
