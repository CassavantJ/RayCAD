// One allocation far larger than the limit, made between two interrupt polls: only the hard limit can stop it.
const s = "x".repeat(100 << 20);
s.length;
