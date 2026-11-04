# lr / sc sequences run as one unit
lr.w a0, (a1) ; sc.w a3, a2, (a1)
lr.w a0, (a1) ; sc.w a3, a2, (a4)  # @state: x14=win+1800
sc.w a3, a2, (a1)
lr.w a0, (a1) ; addi a0, a0, 1 ; sc.w a3, a0, (a1)
