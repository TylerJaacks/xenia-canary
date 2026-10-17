test_lwarx_replaces_reservation:
  #_ MEMORY_IN 10001000 0000000100000000
  #_ REGISTER_IN r3 0x0000000000000005
  #_ REGISTER_IN r4 0x0000000010001000
  # A reservation that is never stored to (compare failed, branched out)...
  lwarx r6, 0, r4
  # ...is replaced by the next reserved load, which then stores normally.
  lwarx r7, 0, r4
  stwcx. r3, 0, r4
  mfcr r12
  blr
  #_ REGISTER_OUT r3 0x0000000000000005
  #_ REGISTER_OUT r4 0x0000000010001000
  #_ REGISTER_OUT r6 0x0000000000000001
  #_ REGISTER_OUT r7 0x0000000000000001
  #_ REGISTER_OUT r12 0x20000000
  #_ MEMORY_OUT 10001000 0000000500000000

test_lwarx_replace_other_block:
  #_ MEMORY_IN 10001000 0000000100000000
  #_ MEMORY_IN 10021000 0000000200000000
  #_ REGISTER_IN r3 0x0000000000000007
  #_ REGISTER_IN r4 0x0000000010001000
  #_ REGISTER_IN r5 0x0000000010021000
  # The second reserved load is in another 64 KB block.
  lwarx r6, 0, r4
  lwarx r7, 0, r5
  stwcx. r3, 0, r5
  mfcr r12
  blr
  #_ REGISTER_OUT r6 0x0000000000000001
  #_ REGISTER_OUT r7 0x0000000000000002
  #_ REGISTER_OUT r12 0x20000000
  #_ MEMORY_OUT 10021000 0000000700000000

test_stwcx_other_block_fails:
  #_ MEMORY_IN 10001000 0000000100000000
  #_ MEMORY_IN 10021000 0000000200000000
  #_ REGISTER_IN r3 0x0000000000000009
  #_ REGISTER_IN r4 0x0000000010001000
  #_ REGISTER_IN r5 0x0000000010021000
  # A conditional store outside the reserved block fails and drops the
  # reservation, so a store to the reserved address fails too.
  lwarx r6, 0, r4
  stwcx. r3, 0, r5
  mfcr r12
  stwcx. r3, 0, r4
  mfcr r13
  blr
  #_ REGISTER_OUT r12 0x00000000
  #_ REGISTER_OUT r13 0x00000000
  #_ MEMORY_OUT 10001000 0000000100000000
  #_ MEMORY_OUT 10021000 0000000200000000
