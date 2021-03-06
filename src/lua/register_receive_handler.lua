return function(ipc, run_hacktimer, pending_alerts_iterator)
  local timer_handler
  timer_handler = function(premature)
    
    for src_slot, src_pid, name, data in pending_alerts_iterator do
      
      ipc.sender = src_pid
      
      local handler = ipc.handlers[name]
      if handler then
        handler(data)
      elseif ipc.default_handler then
        ipc.default_handler(name, data)
      end
    end
    
    ipc.sender = nil
    
    --add timer again and hack it
    run_hacktimer(timer_handler)
  end

  local hacktimer_started = false
  
  local register_handler = function(name, handler, intable)
    if type(name) ~= "string" then
      
      error("bad ".. (intable and "table key in argument #1" or "argument #1") .. " to 'ngx.ipc.receive' (string expected, got " .. type(name) .. ")")
    end
    local handler_type = type(handler)
    if handler_type ~= "function" and handler_type ~= "nil"  then
      error("bad " .. (intable and "table value in argument #1" or "argument #2") .. " to 'ngx.ipc.receive' (function or nil expected, got " .. type(name) .. ")")
    end
    ipc.handlers[name]=handler
  end
  
  return function(name, handler)
    if type(name)~="table" then
      register_handler(name, handler, false)
    else
      if handler ~= nil then
        error("bad argument #2 to 'ngx.ipc.receive' when argument #1 is table (nil expected, got "..type(handler)..")")
      end
      for n, h in pairs(name) do
        register_handler(n, h, true)
      end
    end
    
    if not hacktimer_started then
      run_hacktimer(timer_handler)
      hacktimer_started = true
    end
    
    return true
  end
end
