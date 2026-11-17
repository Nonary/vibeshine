void test_usb_session_guards(){
  need(hash_text(std::string(64,'a'))&&!hash_text(std::string(63,'a'))&&!hash_text(std::string(64,'g')),"Hash guard regression");
  need(controller_text("ROOT\\USB\\0000")&&!controller_text("ROOT\\OTHER\\0000")&&!controller_text("ROOT\\USB\\0000\\extra")&&!controller_text("ROOT\\USB\\"),"Controller guard regression");
  need(quote(L"abc")==L"\"abc\""&&quote(L"a b")==L"\"a b\""&&quote(L"a\\")==L"\"a\\\\\""&&quote(L"a\"b")==L"\"a\\\"b\"","Command line quoting regression");
  for(const json& bad:{json(-1),json(1.5),json(true),json("1")}){bool rejected=false;try{integer(json{{"n",bad}},"n",255);}catch(...){rejected=true;}need(rejected,"Integer coercion accepted");}
  need(integer(json{{"n",255}},"n",255)==255,"Integer upper boundary");bool rejected=false;try{integer(json{{"n",256}},"n",255);}catch(...){rejected=true;}need(rejected,"Integer overflow accepted");
  for(const json& bad:{json(""),json(std::string("a\0b",3)),json("a\nb"),json(3)}){rejected=false;try{text(json{{"s",bad}},"s");}catch(...){rejected=true;}need(rejected,"Binding text coercion accepted");}
#ifdef _WIN32
  for(const auto& [sddl,accepted]:std::array<std::pair<const wchar_t*,bool>,7>{{
      {L"O:SYD:(A;;FA;;;SY)(A;;FA;;;BA)(A;;FRFX;;;BU)",true},
      {L"O:SYD:(A;;FA;;;S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464)(A;OICIIO;GA;;;CO)(A;;FRFX;;;BU)",true},
      {L"O:S-1-5-80-956008885-3418522649-1831038044-1853292631-2271478464D:(A;;FA;;;SY)(A;;FRFX;;;BU)",true},
      {L"O:SYD:(A;;FA;;;SY)(A;;GW;;;BU)",false},
      {L"O:SYD:(A;;FA;;;SY)(A;;GA;;;BU)",false},
      {L"O:SYD:(A;;FA;;;SY)(A;;0x40;;;BU)",false},
      {L"O:BUD:(A;;FA;;;SY)(A;;FRFX;;;BU)",false}
  }}){
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    need(ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl,SDDL_REVISION_1,&descriptor,nullptr),"Payload ACL fixture conversion");
    PSID owner=nullptr;PACL acl=nullptr;BOOL present=FALSE,defaulted=FALSE;
    need(GetSecurityDescriptorOwner(descriptor,&owner,&defaulted)&&GetSecurityDescriptorDacl(descriptor,&present,&acl,&defaulted)&&present,"Payload ACL fixture extraction");
    bool passed=true;try{validate_payload_acl(owner,acl);}catch(...){passed=false;}
    LocalFree(descriptor);need(passed==accepted,"Program Files payload access decision differs");
  }
  {
    private_security security(user_sid());
    const auto name=L"Global\\VibeSpeakersGuardTest_"+random_name();
    auto first=unique_event(name,security);
    bool collision=false;try{auto second=unique_event(name,security);}catch(...){collision=true;}
    need(collision,"A second controller lease was accepted");first.reset();
    auto replacement=unique_event(name,security);need(bool(replacement),"Closed controller lease cannot be reacquired");
  }
  const std::uint64_t example_time=133744112233445566ull;
  std::vector<std::wstring> owner_args{L"bridge.exe"};append_owner_arguments(owner_args,42,example_time);
  need(owner_args==std::vector<std::wstring>{L"bridge.exe",L"--owner-pid",L"42",L"--owner-creation-time",L"133744112233445566"},"USB owner command tuple changed or truncated");
  bool duplicate=false;try{append_owner_arguments(owner_args,42,example_time);}catch(...){duplicate=true;}
  need(duplicate&&owner_args.size()==5,"Duplicate owner tuple accepted");
  for(const auto& invalid:std::array<std::pair<DWORD,std::uint64_t>,3>{{{0,0},{0,example_time},{42,0}}}){
    std::vector<std::wstring> empty;bool refused=false;try{append_owner_arguments(empty,invalid.first,invalid.second);}catch(...){refused=true;}
    need(refused&&empty.empty(),"Incomplete USB owner tuple accepted or partially emitted");
  }
  com_ptr<role_notice> notice;
  notice.p=new role_notice(L"owned-endpoint");
  notice->OnDeviceRemoved(L"other-endpoint");notice->OnDefaultDeviceChanged(eCapture,eConsole,nullptr);
  need(notice->count==0&&WaitForSingleObject(notice->changed.value,0)==WAIT_TIMEOUT,"Unrelated endpoint notification");
  notice->OnDeviceStateChanged(L"OWNED-ENDPOINT",DEVICE_STATE_NOTPRESENT);
  need(notice->count==1&&WaitForSingleObject(notice->changed.value,0)==WAIT_OBJECT_0,"Owned removal notification lost");
  notice->OnDefaultDeviceChanged(eRender,eConsole,nullptr);
  need(notice->count==2&&WaitForSingleObject(notice->changed.value,0)==WAIT_OBJECT_0&&WaitForSingleObject(notice->changed.value,0)==WAIT_TIMEOUT,"Absent render-role wake lost or repeated");
  usb_source_config_t expected;expected.controller_instance="ROOT\\USB\\0000";expected.pipe=R"(\\.\pipe\VibeSpeakersUsb_test)";
  const json valid={{"Schema","vibe-usb-bridge-binding/1"},{"event","binding"},{"qpc_frequency",10000000},{"source_kind","user_mode_usb"},{"pid",123},{"creation_time",456},{"windows_session",1},{"vendor",0x1209},{"product",0x7140},{"channels",12},{"channel_mask",0x2d63f},{"sample_rate",48000},{"container_bits",32},{"valid_bits",24},{"controller_instance",expected.controller_instance},{"pipe_name","VibeSpeakersUsb_test"},{"owner_verified",true},{"owner_pid",GetCurrentProcessId()},{"owner_creation_time",created(GetCurrentProcess())},{"worker_mmcss_registered",true},{"worker_thread_id",999}};
  validate_binding(valid,expected,123,456,1);
  need(profile_product(expected.format)==0x7140,"Original 12-channel PCM24 product changed");
  const std::array<atmos_format_t,4> six_profiles{{{6,0x3f,32,24},{6,0x60f,32,24},{6,0x3f,16,16},{6,0x60f,16,16}}};
  for(const auto& profile:six_profiles){
    auto offered=expected;offered.format=profile;
    auto binding=valid;binding["channels"]=6;binding["channel_mask"]=profile.channel_mask;
    binding["container_bits"]=profile.container_bits;binding["valid_bits"]=profile.valid_bits;
    binding["product"]=profile_product(profile);validate_binding(binding,offered,123,456,1);
    for(const auto& other:six_profiles){
      if(other==profile)continue;
            auto wrong=binding;
      wrong["channel_mask"]=other.channel_mask;
      wrong["container_bits"]=other.container_bits;
      wrong["valid_bits"]=other.valid_bits;
      bool refused=false;
      try{validate_binding(wrong,offered,123,456,1);}catch(...){refused=true;}
      need(refused,"Swapped six-channel layout or precision PID accepted");
    }
  }
  bool unknown_rejected=false;try{profile_product({6,3,32,24});}catch(...){unknown_rejected=true;}
  need(unknown_rejected,"Unknown native format received a product ID");
  const std::array<std::pair<const char*,json>,21> corrupt{{{"Schema","other"},{"event","starting"},{"qpc_frequency",0},{"source_kind","kernel"},{"pid",124},{"creation_time",457},{"windows_session",2},{"vendor",0},{"product",0},{"channels",2},{"channel_mask",3},{"sample_rate",44100},{"container_bits",16},{"valid_bits",16},{"controller_instance","ROOT\\USB\\0001"},{"pipe_name","VibeSpeakersUsb_other"},{"owner_verified",false},{"owner_pid",0},{"owner_creation_time",0},{"worker_mmcss_registered","true"},{"worker_thread_id",0}}};
  for(const auto& change:corrupt){auto bad=valid;bad[change.first]=change.second;rejected=false;try{validate_binding(bad,expected,123,456,1);}catch(...){rejected=true;}need(rejected,"Unbound readiness metadata accepted");}
#endif
}
