// Catastrophic backtracking inside the regexp engine: no JS bytecode runs while it spins.
/^(a+)+$/.test("a".repeat(40) + "!");
