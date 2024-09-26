#!/usr/bin/ruby

# github docs point here. Maybe eventually we'll be able to discover
# it, so that when this path updates we don't have to edit our stuff.
BASE = 'C:\Program Files\Microsoft Visual Studio\2022\Enterprise'

require 'json'

# MSVC and windows stuff is sick. Or maybe I am not aware of
# how to do it nicer. So what happens below?
#
# Well, we need environment variables setup as if vcvars64.bat is run
# (the one that does "cmd with visual studio environment whatever
# stuff"). "Thanks" to madness of cmd and .bat files I see no other
# way than to create .bat file, that will "call" vcvars thingy and
# then invoke ruby which will then dump environment variables we
# need. Then "outer" script handles unmarshalling and caching this.

Dir.chdir(File.dirname(__FILE__)) do
  h = begin
        JSON.parse(IO.read("env-cache"))
      rescue
        nil
      end
  unless h
    File.open("msvc-env.bat", "w") do |f|
      f.write(<<HERE)
@call "#{File.join(BASE, 'VC/Auxiliary/Build/vcvars64.bat')}"
@ruby -rjson -e "puts('!'*32); puts ENV.to_hash.to_json"
HERE
    end
    output = `msvc-env.bat`
    h = JSON.parse(output.split('!'*32, 2).last)
    File.open("env-cache", "w") {|f| f << h.to_json }
  end
  ENV.update(h)
end

# We then exec cmake/ninja as directed with environment variables updated

exec(*ARGV)
