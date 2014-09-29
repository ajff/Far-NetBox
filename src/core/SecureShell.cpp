//---------------------------------------------------------------------------
#include <vcl.h>
#pragma hdrstop

#include "Common.h"
#include "PuttyIntf.h"
#include "Exceptions.h"
#include "Interface.h"
#include "SecureShell.h"
#include "TextsCore.h"
#include "HelpCore.h"
#include "CoreMain.h"
#include <StrUtils.hpp>

#ifndef AUTO_WINSOCK
#include <winsock2.h>
#endif
//---------------------------------------------------------------------------
#define MAX_BUFSIZE 128 * 1024

using namespace Sysutils;
//---------------------------------------------------------------------------
struct TPuttyTranslation
{
  const wchar_t * Original;
  int Translation;
  UnicodeString HelpKeyword;
};
//---------------------------------------------------------------------------
TSecureShell::TSecureShell(TSessionUI * UI,
  TSessionData * SessionData, TSessionLog * Log, TConfiguration * Configuration)
{
  FUI = UI;
  FSessionData = SessionData;
  FLog = Log;
  FConfiguration = Configuration;
  FActive = false;
  FSessionInfoValid = false;
  FBackend = nullptr;
  FSshImplementation = sshiUnknown;
  PendLen = 0;
  PendSize = 0;
  OutLen = 0;
  OutPtr = nullptr;
  Pending = nullptr;
  FBackendHandle = nullptr;
  ResetConnection();
  FOnCaptureOutput = nullptr;
  FOnReceive = nullptr;
  FSocket = INVALID_SOCKET;
  FSocketEvent = ::CreateEvent(nullptr, false, false, nullptr);
  FFrozen = false;
  FDataWhileFrozen = false;
  FSshVersion = 0;
  FOpened = false;
  FWaiting = 0;
  FSimple = false;
  FNoConnectionResponse = false;
  FCollectPrivateKeyUsage = false;
  FWaitingForData = 0;
}
//---------------------------------------------------------------------------
TSecureShell::~TSecureShell()
{
  assert(FWaiting == 0);
  SetActive(false);
  ResetConnection();
  ::CloseHandle(FSocketEvent);
}
//---------------------------------------------------------------------------
void TSecureShell::ResetConnection()
{
  FreeBackend();
  ClearStdError();
  PendLen = 0;
  PendSize = 0;
  sfree(Pending);
  Pending = nullptr;
  FCWriteTemp = L"";
  ResetSessionInfo();
  FAuthenticating = false;
  FAuthenticated = false;
  FStoredPasswordTried = false;
  FStoredPasswordTriedForKI = false;
  FStoredPassphraseTried = false;
}
//---------------------------------------------------------------------------
void TSecureShell::ResetSessionInfo()
{
  FSessionInfoValid = false;
  FMinPacketSize = nullptr;
  FMaxPacketSize = nullptr;
}
//---------------------------------------------------------------------------
void TSecureShell::UpdateSessionInfo() const
{
  if (!FSessionInfoValid)
  {
    FSshVersion = get_ssh_version(FBackendHandle);
    FSessionInfo.ProtocolBaseName = L"SSH";
    FSessionInfo.ProtocolName =
      FORMAT(L"%s-%d", FSessionInfo.ProtocolBaseName.c_str(), get_ssh_version(FBackendHandle));
    FSessionInfo.SecurityProtocolName = FSessionInfo.ProtocolName;

    FSessionInfo.CSCompression =
      FuncToCompression(FSshVersion, get_cscomp(FBackendHandle));
    FSessionInfo.SCCompression =
      FuncToCompression(FSshVersion, get_sccomp(FBackendHandle));

    if (FSshVersion == 1)
    {
      FSessionInfo.CSCipher = CipherNames[FuncToSsh1Cipher(get_cipher(FBackendHandle))];
      FSessionInfo.SCCipher = CipherNames[FuncToSsh1Cipher(get_cipher(FBackendHandle))];
    }
    else
    {
      FSessionInfo.CSCipher = CipherNames[FuncToSsh2Cipher(get_cscipher(FBackendHandle))];
      FSessionInfo.SCCipher = CipherNames[FuncToSsh2Cipher(get_sccipher(FBackendHandle))];
    }

    FSessionInfoValid = true;
  }
}
//---------------------------------------------------------------------------
const TSessionInfo & TSecureShell::GetSessionInfo() const
{
  if (!FSessionInfoValid)
  {
    UpdateSessionInfo();
  }
  return FSessionInfo;
}
//---------------------------------------------------------------------
Conf * TSecureShell::StoreToConfig(TSessionData * Data, bool Simple)
{
  Conf * conf = conf_new();

  assert((asOn == FORCE_ON) && (asOff == FORCE_OFF) && (asAuto == AUTO));

#define CONF_ssh_cipherlist_MAX CIPHER_MAX
#define CONF_DEF_INT_NONE(KEY) conf_set_int(conf, KEY, 0);
#define CONF_DEF_STR_NONE(KEY) conf_set_str(conf, KEY, "");
  // noop, used only for these and we set the first three explicitly below and latter two are not used in our code
#define CONF_DEF_INT_INT(KEY) assert((KEY == CONF_ssh_cipherlist) || (KEY == CONF_ssh_kexlist) || (KEY == CONF_ssh_gsslist) || (KEY == CONF_colours) || (KEY == CONF_wordness));
  // noop, used only for these three and they all can handle undef value
#define CONF_DEF_STR_STR(KEY) assert((KEY == CONF_ttymodes) || (KEY == CONF_portfwd) || (KEY == CONF_environmt));
  // noop, not used in our code
#define CONF_DEF_FONT_NONE(KEY) assert((KEY == CONF_font) || (KEY == CONF_boldfont) || (KEY == CONF_widefont) || (KEY == CONF_wideboldfont));
#define CONF_DEF_FILENAME_NONE(KEY) \
    { \
      Filename * filename = filename_from_str(""); \
      conf_set_filename(conf, KEY, filename); \
      filename_free(filename); \
    }
#define CONF_SET_DEFAULT(VALTYPE, KEYTYPE, KEYWORD) CONF_DEF_ ## VALTYPE ## _ ## KEYTYPE(CONF_ ## KEYWORD);
  CONFIG_OPTIONS(CONF_SET_DEFAULT);
#undef CONF_SET_DEFAULT
#undef CONF_DEF_FILENAME_NONE
#undef CONF_DEF_FONT_NONE
#undef CONF_DEF_STR_STR
#undef CONF_DEF_INT_INT
#undef CONF_DEF_STR_NONE
#undef CONF_DEF_INT_NONE

  // user-configurable settings
  conf_set_str(conf, CONF_host, AnsiString(Data->GetHostNameExpanded()).c_str());
  conf_set_str(conf, CONF_username, AnsiString(Data->GetUserNameExpanded()).c_str());
  conf_set_int(conf, CONF_port, (int)Data->GetPortNumber());
  conf_set_int(conf, CONF_protocol, PROT_SSH);
  // always set 0, as we will handle keepalives ourselves to avoid
  // multi-threaded issues in putty timer list
  conf_set_int(conf, CONF_ping_interval, 0);
  conf_set_int(conf, CONF_compression, Data->GetCompression());
  conf_set_int(conf, CONF_tryagent, Data->GetTryAgent());
  conf_set_int(conf, CONF_agentfwd, Data->GetAgentFwd());
  conf_set_int(conf, CONF_addressfamily, Data->GetAddressFamily());
  conf_set_str(conf, CONF_ssh_rekey_data, AnsiString(Data->GetRekeyData()).c_str());
  conf_set_int(conf, CONF_ssh_rekey_time, (int)Data->GetRekeyTime());

  for (int c = 0; c < CIPHER_COUNT; c++)
  {
    int pcipher = 0;
    switch (Data->GetCipher(c))
    {
      case cipWarn: pcipher = CIPHER_WARN; break;
      case cip3DES: pcipher = CIPHER_3DES; break;
      case cipBlowfish: pcipher = CIPHER_BLOWFISH; break;
      case cipAES: pcipher = CIPHER_AES; break;
      case cipDES: pcipher = CIPHER_DES; break;
      case cipArcfour: pcipher = CIPHER_ARCFOUR; break;
      default: FAIL;
    }
    conf_set_int_int(conf, CONF_ssh_cipherlist, c, pcipher);
  }

  for (int k = 0; k < KEX_COUNT; k++)
  {
    int pkex = 0;
    switch (Data->GetKex(k))
    {
      case kexWarn: pkex = KEX_WARN; break;
      case kexDHGroup1: pkex = KEX_DHGROUP1; break;
      case kexDHGroup14: pkex = KEX_DHGROUP14; break;
      case kexDHGEx: pkex = KEX_DHGEX; break;
      case kexRSA: pkex = KEX_RSA; break;
      default: FAIL;
    }
    conf_set_int_int(conf, CONF_ssh_kexlist, k, pkex);
  }

  UnicodeString SPublicKeyFile = Data->GetPublicKeyFile();
  if (SPublicKeyFile.IsEmpty())
  {
    SPublicKeyFile = GetConfiguration()->GetDefaultKeyFile();
  }
  SPublicKeyFile = StripPathQuotes(ExpandEnvironmentVariables(SPublicKeyFile));
  Filename * KeyFileFileName = filename_from_str(AnsiString(SPublicKeyFile).c_str());
  conf_set_filename(conf, CONF_keyfile, KeyFileFileName);
  filename_free(KeyFileFileName);

  conf_set_int(conf, CONF_sshprot, Data->GetSshProt());
  conf_set_int(conf, CONF_ssh2_des_cbc, Data->GetSsh2DES());
  conf_set_int(conf, CONF_ssh_no_userauth, Data->GetSshNoUserAuth());
  conf_set_int(conf, CONF_try_tis_auth, Data->GetAuthTIS());
  conf_set_int(conf, CONF_try_ki_auth, Data->GetAuthKI());
  conf_set_int(conf, CONF_try_gssapi_auth, Data->GetAuthGSSAPI());
  conf_set_int(conf, CONF_gssapifwd, Data->GetGSSAPIFwdTGT());
  conf_set_int(conf, CONF_change_username, Data->GetChangeUsername());

  conf_set_int(conf, CONF_proxy_type, Data->GetActualProxyMethod());
  conf_set_str(conf, CONF_proxy_host, AnsiString(Data->GetProxyHost()).c_str());
  conf_set_int(conf, CONF_proxy_port, static_cast<int>(Data->GetProxyPort()));
  conf_set_str(conf, CONF_proxy_username, AnsiString(Data->GetProxyUsername()).c_str());
  conf_set_str(conf, CONF_proxy_password, AnsiString(Data->GetProxyPassword()).c_str());
  if (Data->GetProxyMethod() == pmCmd)
  {
    conf_set_str(conf, CONF_proxy_telnet_command, AnsiString(Data->GetProxyLocalCommand()).c_str());
  }
  else
  {
    conf_set_str(conf, CONF_proxy_telnet_command, AnsiString(Data->GetProxyTelnetCommand()).c_str());
  }
  conf_set_int(conf, CONF_proxy_dns, Data->GetProxyDNS());
  conf_set_int(conf, CONF_even_proxy_localhost, Data->GetProxyLocalhost());

  conf_set_int(conf, CONF_sshbug_ignore1, Data->GetBug(sbIgnore1));
  conf_set_int(conf, CONF_sshbug_plainpw1, Data->GetBug(sbPlainPW1));
  conf_set_int(conf, CONF_sshbug_rsa1, Data->GetBug(sbRSA1));
  conf_set_int(conf, CONF_sshbug_hmac2, Data->GetBug(sbHMAC2));
  conf_set_int(conf, CONF_sshbug_derivekey2, Data->GetBug(sbDeriveKey2));
  conf_set_int(conf, CONF_sshbug_rsapad2, Data->GetBug(sbRSAPad2));
  conf_set_int(conf, CONF_sshbug_rekey2, Data->GetBug(sbRekey2));
  conf_set_int(conf, CONF_sshbug_pksessid2, Data->GetBug(sbPKSessID2));
  conf_set_int(conf, CONF_sshbug_maxpkt2, Data->GetBug(sbMaxPkt2));
  conf_set_int(conf, CONF_sshbug_ignore2, Data->GetBug(sbIgnore2));
  conf_set_int(conf, CONF_sshbug_winadj, Data->GetBug(sbWinAdj));

  if (!Data->GetTunnelPortFwd().IsEmpty())
  {
    assert(!Simple);
    UnicodeString TunnelPortFwd = Data->GetTunnelPortFwd();
    while (!TunnelPortFwd.IsEmpty())
    {
      UnicodeString Buf = CutToChar(TunnelPortFwd, L',', true);
      AnsiString Key = AnsiString(CutToChar(Buf, L'\t', true));
      AnsiString Value = AnsiString(Buf);
      conf_set_str_str(conf, CONF_portfwd, Key.c_str(), Value.c_str());
    }

    // when setting up a tunnel, do not open shell/sftp
    conf_set_int(conf, CONF_ssh_no_shell, TRUE);
  }
  else
  {
    assert(Simple);
    conf_set_int(conf, CONF_ssh_simple, Data->GetSshSimple() && Simple);

    if (Data->GetFSProtocol() == fsSCPonly)
    {
      conf_set_int(conf, CONF_ssh_subsys, FALSE);
      if (Data->GetShell().IsEmpty())
      {
        // Following forces Putty to open default shell
        // see ssh.c: do_ssh2_authconn() and ssh1_protocol()
        conf_set_str(conf, CONF_remote_cmd, "");
      }
      else
      {
        conf_set_str(conf, CONF_remote_cmd, AnsiString(Data->GetShell().c_str()).c_str());
      }
    }
    else
    {
      if (Data->GetSftpServer().IsEmpty())
      {
        conf_set_int(conf, CONF_ssh_subsys, TRUE);
        conf_set_str(conf, CONF_remote_cmd, "sftp");
      }
      else
      {
        conf_set_int(conf, CONF_ssh_subsys, FALSE);
        conf_set_str(conf, CONF_remote_cmd, AnsiString(Data->GetSftpServer().c_str()).c_str());
      }

      if (Data->GetFSProtocol() != fsSFTPonly)
      {
        conf_set_int(conf, CONF_ssh_subsys2, FALSE);
        if (Data->GetShell().IsEmpty())
        {
          // Following forces Putty to open default shell
          // see ssh.c: do_ssh2_authconn() and ssh1_protocol()
          conf_set_str(conf, CONF_remote_cmd2, "");
          // PuTTY ignores CONF_remote_cmd2 set to "",
          // so we have to enforce it
          // (CONF_force_remote_cmd2 is our config option)
          conf_set_int(conf, CONF_force_remote_cmd2, 1);
        }
        else
        {
          conf_set_str(conf, CONF_remote_cmd2, AnsiString(Data->GetShell().c_str()).c_str());
        }
      }

      if ((Data->GetFSProtocol() == fsSFTPonly) && Data->GetSftpServer().IsEmpty())
      {
        // see psftp_connect() from psftp.c
        conf_set_int(conf, CONF_ssh_subsys2, FALSE);
        conf_set_str(conf, CONF_remote_cmd2,
          "test -x /usr/lib/sftp-server && exec /usr/lib/sftp-server\n"
          "test -x /usr/local/lib/sftp-server && exec /usr/local/lib/sftp-server\n"
          "exec sftp-server");
      }
    }
  }

  conf_set_int(conf, CONF_connect_timeout, static_cast<int>(Data->GetTimeout() * MSecsPerSec));
  conf_set_int(conf, CONF_sndbuf, static_cast<int>(Data->GetSendBuf()));

  // permanent settings
  conf_set_int(conf, CONF_nopty, TRUE);
  conf_set_int(conf, CONF_tcp_keepalives, 0);
  conf_set_int(conf, CONF_ssh_show_banner, TRUE);
  for (int Index = 0; Index < ngsslibs; ++Index)
  {
    conf_set_int_int(conf, CONF_ssh_gsslist, Index, gsslibkeywords[Index].v);
  }
  return conf;
}
//---------------------------------------------------------------------------
void TSecureShell::Open()
{
  FBackend = &ssh_backend;
  ResetConnection();

  FAuthenticating = false;
  FAuthenticated = false;

  SetActive(false);

  FAuthenticationLog = L"";
  FNoConnectionResponse = false;
  FUI->Information(LoadStr(STATUS_LOOKUPHOST), true);

  try
  {
    char * RealHost = nullptr;
    FreeBackend(); // in case we are reconnecting
    const char * InitError = nullptr;
    Conf * conf = StoreToConfig(FSessionData, GetSimple());
    {
      SCOPE_EXIT
      {
        conf_free(conf);
      };
      InitError = FBackend->init(this, &FBackendHandle, conf,
        AnsiString(FSessionData->GetHostNameExpanded()).c_str(), (int)FSessionData->GetPortNumber(), &RealHost, 0,
        conf_get_int(conf, CONF_tcp_keepalives));
    }

    sfree(RealHost);
    if (InitError)
    {
      PuttyFatalError(UnicodeString(InitError));
    }
    FUI->Information(LoadStr(STATUS_CONNECT), true);
    Init();

    CheckConnection(CONNECTION_FAILED);
  }
  catch (Exception & E)
  {
    if (FNoConnectionResponse && TryFtp())
    {
      // GetConfiguration()->Usage->Inc(L"ProtocolSuggestions");
      // HELP_FTP_SUGGESTION won't be used as all errors that set
      // FNoConnectionResponse have already their own help keyword
      FUI->FatalError(&E, LoadStr(FTP_SUGGESTION));
    }
    else
    {
      throw;
    }
  }
  FLastDataSent = Now();

  FSessionInfo.LoginTime = Now();

  FAuthenticating = false;
  FAuthenticated = true;
  FUI->Information(LoadStr(STATUS_AUTHENTICATED), true);

  ResetSessionInfo();

  assert(!FSessionInfo.SshImplementation.IsEmpty());
  FOpened = true;

  UnicodeString SshImplementation = GetSessionInfo().SshImplementation;
  if (// e.g. "OpenSSH_5.3"
    (SshImplementation.Pos(L"OpenSSH") == 1) ||
      // Sun SSH is based on OpenSSH (suffers the same bugs)
      (SshImplementation.Pos(L"Sun_SSH") == 1))
  {
    FSshImplementation = sshiOpenSSH;
  }
  // e.g. "mod_sftp/0.9.8"
  else if (SshImplementation.Pos(L"mod_sftp") == 1)
  {
    FSshImplementation = sshiProFTPD;
  }
  // e.g. "5.25 FlowSsh: Bitvise SSH Server (WinSSHD) 6.07: free only for personal non-commercial use"
  else if (SshImplementation.Pos(L"FlowSsh") > 0)
  {
    FSshImplementation = sshiBitvise;
  }
  else
  {
    FSshImplementation = sshiUnknown;
  }
}
//---------------------------------------------------------------------------
bool TSecureShell::TryFtp()
{
  bool Result;
  if (!FConfiguration->GetTryFtpWhenSshFails())
  {
    Result = false;
  }
  else
  {
    if (((FSessionData->GetFSProtocol() != fsSFTP) && (FSessionData->GetFSProtocol() != fsSFTPonly)) ||
        (FSessionData->GetPortNumber() != SshPortNumber) ||
        FSessionData->GetTunnel() || (FSessionData->GetProxyMethod() != ::pmNone))
    {
      LogEvent(L"Using non-standard protocol or port, tunnel or proxy, will not knock FTP port.");
      Result = false;
    }
    else
    {
      LogEvent(L"Knocking FTP port.");

      SOCKET Socket = socket(AF_INET, SOCK_STREAM, 0);
      Result = (Socket != INVALID_SOCKET);
      if (Result)
      {
        LPHOSTENT HostEntry = gethostbyname(AnsiString(FSessionData->GetHostNameExpanded()).c_str());
        Result = (HostEntry != nullptr);
        if (Result)
        {
          SOCKADDR_IN Address;

          memset(&Address, 0, sizeof(Address));
          Address.sin_family = AF_INET;
          intptr_t Port = FtpPortNumber;
          Address.sin_port = htons(static_cast<short>(Port));
          Address.sin_addr.s_addr = *((uint32_t *)*HostEntry->h_addr_list);

          HANDLE Event = ::CreateEvent(nullptr, false, false, nullptr);
          Result = (::WSAEventSelect(Socket, (WSAEVENT)Event, FD_CONNECT | FD_CLOSE) != SOCKET_ERROR);

          if (Result)
          {
            Result =
              (connect(Socket, reinterpret_cast<sockaddr *>(&Address), sizeof(Address)) != SOCKET_ERROR) ||
              (::WSAGetLastError() == WSAEWOULDBLOCK);
            if (Result)
            {
              Result = (::WaitForSingleObject(Event, 2000) == WAIT_OBJECT_0);
            }
          }
          ::CloseHandle(Event);
        }
        closesocket(Socket);
      }

      if (Result)
      {
        LogEvent(L"FTP port opened, will suggest using FTP protocol.");
      }
      else
      {
        LogEvent(L"FTP port did not open.");
      }
    }
  }
  return Result;
}
//---------------------------------------------------------------------------
void TSecureShell::Init()
{
  try
  {
    try
    {
      // Recent pscp checks FBackend->exitcode(FBackendHandle) in the loop
      // (see comment in putty revision 8110)
      // It seems that we do not need to do it.

      while (!get_ssh_state_session(FBackendHandle))
      {
        if (GetConfiguration()->GetActualLogProtocol() >= 1)
        {
          LogEvent(L"Waiting for the server to continue with the initialization");
        }
        WaitForData();
      }

      // unless this is tunnel session, it must be safe to send now
      assert(FBackend->sendok(FBackendHandle) || !FSessionData->GetTunnelPortFwd().IsEmpty());
    }
    catch (Exception & E)
    {
      if (FAuthenticating && !FAuthenticationLog.IsEmpty())
      {
        FUI->FatalError(&E, FMTLOAD(AUTHENTICATION_LOG, FAuthenticationLog.c_str()));
      }
      else
      {
        throw;
      }
    }
  }
  catch (Exception & E)
  {
    if (FAuthenticating)
    {
      FUI->FatalError(&E, LoadStr(AUTHENTICATION_FAILED));
    }
    else
    {
      throw;
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::PuttyLogEvent(const UnicodeString & Str)
{
#define SERVER_VERSION_MSG L"Server version: "
  // Gross hack
  if (Str.Pos(SERVER_VERSION_MSG) == 1)
  {
    FSessionInfo.SshVersionString = Str.SubString(wcslen(SERVER_VERSION_MSG) + 1,
      Str.Length() - wcslen(SERVER_VERSION_MSG));

    const wchar_t * Ptr = wcschr(FSessionInfo.SshVersionString.c_str(), L'-');
    if (Ptr != nullptr)
    {
      Ptr = wcschr(Ptr + 1, L'-');
    }
    FSessionInfo.SshImplementation = (Ptr != nullptr) ? Ptr + 1 : L"";
  }
#define FORWARDING_FAILURE_MSG L"Forwarded connection refused by server: "
  else if (Str.Pos(FORWARDING_FAILURE_MSG) == 1)
  {
    FLastTunnelError = Str.SubString(wcslen(FORWARDING_FAILURE_MSG) + 1,
      Str.Length() - wcslen(FORWARDING_FAILURE_MSG));

    static const TPuttyTranslation Translation[] =
    {
      { L"Administratively prohibited [%]", PFWD_TRANSL_ADMIN },
      { L"Connect failed [%]", PFWD_TRANSL_CONNECT },
    };
    TranslatePuttyMessage(Translation, LENOF(Translation), FLastTunnelError);
  }
  LogEvent(Str);
}
//---------------------------------------------------------------------------
bool TSecureShell::PromptUser(bool /*ToServer*/,
  const UnicodeString & AName, bool /*NameRequired*/,
  const UnicodeString & Instructions, bool InstructionsRequired,
  TStrings * Prompts, TStrings * Results)
{
  // there can be zero prompts!

  assert(Results->GetCount() == Prompts->GetCount());

  TPromptKind PromptKind;
  // beware of changing order
  static const TPuttyTranslation NameTranslation[] =
  {
    { L"SSH login name", USERNAME_TITLE },
    { L"SSH key passphrase", PASSPHRASE_TITLE },
    { L"SSH TIS authentication", SERVER_PROMPT_TITLE },
    { L"SSH CryptoCard authentication", SERVER_PROMPT_TITLE },
    { L"SSH server: %", SERVER_PROMPT_TITLE2 },
    { L"SSH server authentication", SERVER_PROMPT_TITLE },
    { L"SSH password", PASSWORD_TITLE },
    { L"New SSH password", NEW_PASSWORD_TITLE },
  };

  UnicodeString Name = AName;
  int Index = TranslatePuttyMessage(NameTranslation, LENOF(NameTranslation), Name);

  const TPuttyTranslation * InstructionTranslation = nullptr;
  const TPuttyTranslation * PromptTranslation = nullptr;
  size_t PromptTranslationCount = 1;
  UnicodeString PromptDesc;

  if (Index == 0) // username
  {
    static const TPuttyTranslation UsernamePromptTranslation[] =
    {
      { L"login as: ", USERNAME_PROMPT2 },
    };

    PromptTranslation = UsernamePromptTranslation;
    PromptKind = pkUserName;
    PromptDesc = L"username";
  }
  else if (Index == 1) // passphrase
  {
    static const TPuttyTranslation PassphrasePromptTranslation[] =
    {
      { L"Passphrase for key \"%\": ", PROMPT_KEY_PASSPHRASE },
    };

    PromptTranslation = PassphrasePromptTranslation;
    PromptKind = pkPassphrase;
    PromptDesc = L"passphrase";
  }
  else if (Index == 2) // TIS
  {
    static const TPuttyTranslation TISInstructionTranslation[] =
    {
      { L"Using TIS authentication.%", TIS_INSTRUCTION },
    };
    static const TPuttyTranslation TISPromptTranslation[] =
    {
      { L"Response: ", PROMPT_PROMPT },
    };

    InstructionTranslation = TISInstructionTranslation;
    PromptTranslation = TISPromptTranslation;
    PromptKind = pkTIS;
    PromptDesc = L"tis";
  }
  else if (Index == 3) // CryptoCard
  {
    static const TPuttyTranslation CryptoCardInstructionTranslation[] =
    {
      { L"Using CryptoCard authentication.%", CRYPTOCARD_INSTRUCTION },
    };
    static const TPuttyTranslation CryptoCardPromptTranslation[] =
    {
      { L"Response: ", PROMPT_PROMPT },
    };

    InstructionTranslation = CryptoCardInstructionTranslation;
    PromptTranslation = CryptoCardPromptTranslation;
    PromptKind = pkCryptoCard;
    PromptDesc = L"cryptocard";
  }
  else if ((Index == 4) || (Index == 5))
  {
    static const TPuttyTranslation KeybInteractiveInstructionTranslation[] =
    {
      { L"Using keyboard-interactive authentication.%", KEYBINTER_INSTRUCTION },
    };
    static const TPuttyTranslation KeybInteractivePromptTranslation[] =
    {
      // as used by Linux-PAM (pam_exec/pam_exec.c, libpam/pam_get_authtok.c,
      // pam_unix/pam_unix_auth.c, pam_userdb/pam_userdb.c)
      { L"Password: ", PASSWORD_PROMPT },
    };

    InstructionTranslation = KeybInteractiveInstructionTranslation;
    PromptTranslation = KeybInteractivePromptTranslation;
    PromptKind = pkKeybInteractive;
    PromptDesc = L"keyboard interactive";
  }
  else if (Index == 6)
  {
    assert(Prompts->GetCount() == 1);
    Prompts->SetString(0, LoadStr(PASSWORD_PROMPT));
    PromptKind = pkPassword;
    PromptDesc = L"password";
  }
  else if (Index == 7)
  {
    static const TPuttyTranslation NewPasswordPromptTranslation[] =
    {
      { L"Current password (blank for previously entered password): ", NEW_PASSWORD_CURRENT_PROMPT },
      { L"Enter new password: ", NEW_PASSWORD_NEW_PROMPT },
      { L"Confirm new password: ", NEW_PASSWORD_CONFIRM_PROMPT },
    };
    PromptTranslation = NewPasswordPromptTranslation;
    PromptTranslationCount = LENOF(NewPasswordPromptTranslation);
    PromptKind = pkNewPassword;
    PromptDesc = L"new password";
  }
  else
  {
    PromptKind = pkPrompt;
    PromptDesc = L"unknown";
    FAIL;
  }

  UnicodeString InstructionsLog =
    (Instructions.IsEmpty() ? UnicodeString(L"<no instructions>") : FORMAT(L"\"%s\"", Instructions.c_str()));
  UnicodeString PromptsLog =
    (Prompts->GetCount() > 0 ? FORMAT(L"\"%s\"", Prompts->GetString(0).c_str()) : UnicodeString(L"<no prompt>")) +
    (Prompts->GetCount() > 1 ? FORMAT(L"%d more", Prompts->GetCount() - 1) : UnicodeString());
  LogEvent(FORMAT(L"Prompt (%s, \"%s\", %s, %s)", PromptDesc.c_str(), AName.c_str(), InstructionsLog.c_str(), PromptsLog.c_str()));

  Name = Name.Trim();

  UnicodeString Instructions2 = Sysutils::ReplaceStrAll(Instructions, L"\x0D\x0A", L"\x01");
  Instructions2 = Sysutils::ReplaceStrAll(Instructions2, L"\x0A\x0D", L"\x01");
  Instructions2 = Sysutils::ReplaceStrAll(Instructions2, L"\x0A", L"\x01");
  Instructions2 = Sysutils::ReplaceStrAll(Instructions2, L"\x0D", L"\x01");
  Instructions2 = Sysutils::ReplaceStrAll(Instructions2, L"\x01", L"\x0D\x0A");
  if (InstructionTranslation != nullptr)
  {
    TranslatePuttyMessage(InstructionTranslation, 1, Instructions2);
  }

  // some servers add leading blank line to make the prompt look prettier
  // on terminal console
  Instructions2 = Instructions2.Trim();

  for (intptr_t Index = 0; Index < Prompts->GetCount(); ++Index)
  {
    UnicodeString Prompt = Prompts->GetString(Index);
    if (PromptTranslation != nullptr)
    {
      TranslatePuttyMessage(PromptTranslation, PromptTranslationCount, Prompt);
    }
    // some servers add leading blank line to make the prompt look prettier
    // on terminal console
    Prompts->SetString(Index, Prompt.Trim());
  }

  bool Result = false;
  if (PromptKind == pkUserName)
  {
    if (FSessionData->GetAuthGSSAPI())
    {
      // use empty username if no username was filled on login dialog
      // and GSSAPI auth is enabled, hence there's chance that the server can
      // deduce the username otherwise
      Results->SetString(0, L"");
      Result = true;
    }
  }
  else if ((PromptKind == pkTIS) || (PromptKind == pkCryptoCard) ||
      (PromptKind == pkKeybInteractive))
  {
    if (FSessionData->GetAuthKIPassword() && !FSessionData->GetPassword().IsEmpty() &&
        !FStoredPasswordTriedForKI && (Prompts->GetCount() == 1) &&
        FLAGCLEAR((intptr_t)Prompts->GetObject(0), pupEcho))
    {
      LogEvent(L"Using stored password.");
      FUI->Information(LoadStr(AUTH_PASSWORD), false);
      Result = true;
      Results->SetString(0, FSessionData->GetPassword());
      FStoredPasswordTriedForKI = true;
    }
    else if (Instructions2.IsEmpty() && !InstructionsRequired && (Prompts->GetCount() == 0))
    {
      LogEvent(L"Ignoring empty SSH server authentication request");
      Result = true;
    }
  }
  else if (PromptKind == pkPassword)
  {
    if (!FSessionData->GetPassword().IsEmpty() && !FStoredPasswordTried)
    {
      LogEvent(L"Using stored password.");
      FUI->Information(LoadStr(AUTH_PASSWORD), false);
      Result = true;
      Results->SetString(0, FSessionData->GetPassword());
      FStoredPasswordTried = true;
    }
  }
  else if (PromptKind == pkPassphrase)
  {
    if (!FSessionData->GetPassphrase().IsEmpty() && !FStoredPassphraseTried)
    {
      LogEvent(L"Using configured passphrase.");
      Result = true;
      Results->SetString(0, FSessionData->GetPassphrase());
      FStoredPassphraseTried = true;
    }
  }

  if (!Result)
  {
    Result = FUI->PromptUser(FSessionData,
      PromptKind, Name, Instructions2, Prompts, Results);

    if (Result)
    {
      if ((Prompts->GetCount() >= 1) && FLAGSET((intptr_t)Prompts->GetObject(0), pupEcho))
      {
        LogEvent(FORMAT(L"Response: \"%s\"", Results->GetString(0).c_str()));
      }

      if ((PromptKind == pkUserName) && (Prompts->GetCount() == 1))
      {
        FUserName = Results->GetString(0);
      }
    }
  }

  return Result;
}
//---------------------------------------------------------------------------
void TSecureShell::GotHostKey()
{
  // due to re-key GotHostKey() may be called again later during session
  if (!FAuthenticating && !FAuthenticated)
  {
    FAuthenticating = true;
    FUI->Information(LoadStr(STATUS_AUTHENTICATE), true);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::CWrite(const char * Data, intptr_t Length)
{
  // some messages to stderr may indicate that something has changed with the
  // session, so reset the session info
  ResetSessionInfo();

  // We send only whole line at once, so we have to cache incoming data
  FCWriteTemp += DeleteChar(UnicodeString(Data, Length), L'\r');

  UnicodeString Line;
  // Do we have at least one complete line in std error cache?
  while (FCWriteTemp.Pos(L"\n") > 0)
  {
    UnicodeString Line = CutToChar(FCWriteTemp, L'\n', false);

    FLog->Add(llStdError, Line);

    if (FAuthenticating)
    {
      TranslateAuthenticationMessage(Line);
      FAuthenticationLog += (FAuthenticationLog.IsEmpty() ? L"" : L"\n") + Line;
    }

    FUI->Information(Line, false);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::RegisterReceiveHandler(TNotifyEvent Handler)
{
  assert(FOnReceive == nullptr);
  FOnReceive = Handler;
}
//---------------------------------------------------------------------------
void TSecureShell::UnregisterReceiveHandler(TNotifyEvent Handler)
{
  assert(FOnReceive == Handler);
  USEDPARAM(Handler);
  FOnReceive = nullptr;
}
//---------------------------------------------------------------------------
void TSecureShell::FromBackend(bool IsStdErr, const uint8_t * Data, intptr_t Length)
{
  CheckConnection();

  if (GetConfiguration()->GetActualLogProtocol() >= 1)
  {
    LogEvent(FORMAT(L"Received %u bytes (%d)", Length, static_cast<int>(IsStdErr)));
  }

  // Following is taken from scp.c from_backend() and modified

  if (IsStdErr)
  {
    AddStdError(UnicodeString(reinterpret_cast<const char *>(Data), Length));
  }
  else
  {
    const uint8_t * p = Data;
    intptr_t Len = Length;

    // with event-select mechanism we can now receive data even before we
    // actually expect them (OutPtr can be nullptr)

    if ((OutPtr != nullptr) && (OutLen > 0) && (Len > 0))
    {
      intptr_t Used = OutLen;
      if (Used > Len)
      {
        Used = Len;
      }
      memmove(OutPtr, p, Used);
      OutPtr += Used; OutLen -= Used;
      p += Used; Len -= Used;
    }

    if (Len > 0)
    {
      if (PendSize < PendLen + Len)
      {
        PendSize = PendLen + Len + 4096;
        Pending = static_cast<uint8_t *>
          (Pending ? srealloc(Pending, PendSize) : smalloc(PendSize));
        if (!Pending)
        {
          FatalError(L"Out of memory");
        }
      }
      memmove(Pending + PendLen, p, Len);
      PendLen += Len;
    }

    if (FOnReceive != nullptr)
    {
      if (!FFrozen)
      {
        FFrozen = true;
        {
          SCOPE_EXIT
          {
            FFrozen = false;
          };
          do
          {
            FDataWhileFrozen = false;
            FOnReceive(nullptr);
          }
          while (FDataWhileFrozen);
        }
      }
      else
      {
        FDataWhileFrozen = true;
      }
    }
  }
}
//---------------------------------------------------------------------------
bool TSecureShell::Peek(uint8_t *& Buf, intptr_t Length) const
{
  bool Result = (PendLen >= Length);

  if (Result)
  {
    Buf = Pending;
  }

  return Result;
}
//---------------------------------------------------------------------------
intptr_t TSecureShell::Receive(uint8_t * Buf, intptr_t Length)
{
  CheckConnection();

  if (Length > 0)
  {
    // Following is taken from scp.c ssh_scp_recv() and modified

    OutPtr = Buf;
    OutLen = Length;

    {
      SCOPE_EXIT
      {
        OutPtr = nullptr;
      };
      /*
       * See if the pending-input block contains some of what we
       * need.
       */
      if (PendLen > 0)
      {
        intptr_t PendUsed = PendLen;
        if (PendUsed > OutLen)
        {
          PendUsed = OutLen;
        }
        memmove(OutPtr, Pending, PendUsed);
        memmove(Pending, Pending + PendUsed, PendLen - PendUsed);
        OutPtr += PendUsed;
        OutLen -= PendUsed;
        PendLen -= PendUsed;
        if (PendLen == 0)
        {
          PendSize = 0;
          sfree(Pending);
          Pending = nullptr;
        }
      }

      while (OutLen > 0)
      {
        if (GetConfiguration()->GetActualLogProtocol() >= 1)
        {
          LogEvent(FORMAT(L"Waiting for another %u bytes", static_cast<int>(OutLen)));
        }
        WaitForData();
      }

      // This seems ambiguous
//      if (Length <= 0)
//      {
//        FatalError(LoadStr(LOST_CONNECTION));
//      }
    }
  }
  if (GetConfiguration()->GetActualLogProtocol() >= 1)
  {
    LogEvent(FORMAT(L"Read %d bytes (%d pending)",
      static_cast<int>(Length), static_cast<int>(PendLen)));
  }
  return Length;
}
//---------------------------------------------------------------------------
UnicodeString TSecureShell::ReceiveLine()
{
  intptr_t Index = 0;
  AnsiString Line;
  Boolean EOL = False;

  do
  {
    // If there is any buffer of received chars
    if (PendLen > 0)
    {
      Index = 0;
      // Repeat until we walk thru whole buffer or reach end-of-line
      while ((Index < PendLen) && (!Index || (Pending[Index - 1] != '\n')))
      {
        ++Index;
      }
      EOL = static_cast<Boolean>(Index && (Pending[Index - 1] == '\n'));
      intptr_t PrevLen = Line.Length();
      Line.SetLength(PrevLen + Index);
      Receive(reinterpret_cast<uint8_t *>(const_cast<char *>(Line.c_str()) + PrevLen), Index);
    }

    // If buffer don't contain end-of-line character
    // we read one more which causes receiving new buffer of chars
    if (!EOL)
    {
      uint8_t Ch;
      Receive(&Ch, 1);
      Line += static_cast<char>(Ch);
      EOL = (static_cast<char>(Ch) == '\n');
    }
  }
  while (!EOL);

  // We don't want end-of-line character
  Line.SetLength(Line.Length() - 1);

  UnicodeString UnicodeLine = Sysutils::TrimRight(Sysutils::MB2W(Line.c_str(), (UINT)FSessionData->GetCodePageAsNumber()));
  CaptureOutput(llOutput, UnicodeLine);
  return UnicodeLine;
}
//---------------------------------------------------------------------------
void TSecureShell::SendSpecial(int Code)
{
  LogEvent(FORMAT(L"Sending special code: %d", Code));
  CheckConnection();
  FBackend->special(FBackendHandle, static_cast<Telnet_Special>(Code));
  CheckConnection();
  FLastDataSent = Now();
}
//---------------------------------------------------------------------------
void TSecureShell::SendEOF()
{
  SendSpecial(TS_EOF);
}
//---------------------------------------------------------------------------
uintptr_t TSecureShell::TimeoutPrompt(TQueryParamsTimerEvent PoolEvent)
{
  FWaiting++;

  uintptr_t Answer = 0;
  {
    SCOPE_EXIT
    {
      FWaiting--;
    };
    TQueryParams Params(qpFatalAbort | qpAllowContinueOnError | qpIgnoreAbort);
    Params.HelpKeyword = HELP_MESSAGE_HOST_IS_NOT_COMMUNICATING;
    Params.Timer = 500;
    Params.TimerEvent = PoolEvent;
    Params.TimerMessage = MainInstructionsFirstParagraph(FMTLOAD(TIMEOUT_STILL_WAITING3, FSessionData->GetTimeout()));
    Params.TimerAnswers = qaAbort;
    Params.TimerQueryType = qtInformation;
    if (FConfiguration->GetSessionReopenAutoStall() > 0)
    {
      Params.Timeout = FConfiguration->GetSessionReopenAutoStall();
      Params.TimeoutAnswer = qaAbort;
    }
    Answer = FUI->QueryUser(MainInstructions(FMTLOAD(CONFIRM_PROLONG_TIMEOUT3, FSessionData->GetTimeout(), FSessionData->GetTimeout())),
      nullptr, qaRetry | qaAbort, &Params);
  }
  return Answer;
}
//---------------------------------------------------------------------------
void TSecureShell::SendBuffer(intptr_t & Result)
{
  // for comments see PoolForData
  if (!GetActive())
  {
    Result = qaRetry;
  }
  else
  {
    try
    {
      if (FBackend->sendbuffer(FBackendHandle) <= MAX_BUFSIZE)
      {
        Result = qaOK;
      }
    }
    catch (...)
    {
      Result = qaRetry;
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::DispatchSendBuffer(intptr_t BufSize)
{
  TDateTime Start = Now();
  do
  {
    CheckConnection();
    if (GetConfiguration()->GetActualLogProtocol() >= 1)
    {
      LogEvent(FORMAT(L"There are %u bytes remaining in the send buffer, "
        L"need to send at least another %u bytes",
        BufSize, BufSize - MAX_BUFSIZE));
    }
    EventSelectLoop(100, false, nullptr);
    BufSize = FBackend->sendbuffer(FBackendHandle);
    if (GetConfiguration()->GetActualLogProtocol() >= 1)
    {
      LogEvent(FORMAT(L"There are %u bytes remaining in the send buffer", BufSize));
    }

    if (Now() - Start > FSessionData->GetTimeoutDT())
    {
      LogEvent(L"Waiting for dispatching send buffer timed out, asking user what to do.");
      uintptr_t Answer = TimeoutPrompt(MAKE_CALLBACK(TSecureShell::SendBuffer, this));
      switch (Answer)
      {
        case qaRetry:
          Start = Now();
          break;

        case qaOK:
          BufSize = 0;
          break;

        default:
          FAIL;
          // fallthru

        case qaAbort:
          FatalError(MainInstructions(LoadStr(USER_TERMINATED)));
          break;
      }
    }
  }
  while (BufSize > MAX_BUFSIZE);
}
//---------------------------------------------------------------------------
void TSecureShell::Send(const uint8_t * Buf, intptr_t Length)
{
  CheckConnection();
  int BufSize = FBackend->send(FBackendHandle, const_cast<char *>(reinterpret_cast<const char *>(Buf)), static_cast<int>(Length));
  if (GetConfiguration()->GetActualLogProtocol() >= 1)
  {
    LogEvent(FORMAT(L"Sent %d bytes", static_cast<int>(Length)));
    LogEvent(FORMAT(L"There are %u bytes remaining in the send buffer", BufSize));
  }
  FLastDataSent = Now();
  // among other forces receive of pending data to free the servers's send buffer
  EventSelectLoop(0, false, nullptr);

  if (BufSize > MAX_BUFSIZE)
  {
    DispatchSendBuffer(BufSize);
  }
  CheckConnection();
}
//---------------------------------------------------------------------------
void TSecureShell::SendNull()
{
  LogEvent(L"Sending nullptr.");
  uint8_t Null = 0;
  Send(&Null, 1);
}
//---------------------------------------------------------------------------
void TSecureShell::SendStr(const UnicodeString & Str)
{
  CheckConnection();
  AnsiString AnsiStr = Sysutils::W2MB(Str.c_str(), (UINT)FSessionData->GetCodePageAsNumber());
  Send(reinterpret_cast<const uint8_t *>(AnsiStr.c_str()), AnsiStr.Length());
}
//---------------------------------------------------------------------------
void TSecureShell::SendLine(const UnicodeString & Line)
{
  SendStr(Line);
  Send(reinterpret_cast<const uint8_t *>("\n"), 1);
  FLog->Add(llInput, Line);
}
//---------------------------------------------------------------------------
int TSecureShell::TranslatePuttyMessage(
  const TPuttyTranslation * Translation, intptr_t Count, UnicodeString & Message,
  UnicodeString * HelpKeyword) const
{
  int Result = -1;
  for (intptr_t Index = 0; Index < Count; ++Index)
  {
    const wchar_t * Original = Translation[Index].Original;
    const wchar_t * Div = wcschr(Original, L'%');
    if (Div == nullptr)
    {
      if (wcscmp(Message.c_str(), Original) == 0)
      {
        Message = LoadStr(Translation[Index].Translation);
        Result = static_cast<int>(Index);
        break;
      }
    }
    else
    {
      size_t OriginalLen = wcslen(Original);
      size_t PrefixLen = Div - Original;
      size_t SuffixLen = OriginalLen - PrefixLen - 1;
      if ((static_cast<size_t>(Message.Length()) >= OriginalLen - 1) &&
          (wcsncmp(Message.c_str(), Original, PrefixLen) == 0) &&
          (wcsncmp(Message.c_str() + Message.Length() - SuffixLen, Div + 1, SuffixLen) == 0))
      {
        Message = FMTLOAD(Translation[Index].Translation,
          Message.SubString(PrefixLen + 1, Message.Length() - PrefixLen - SuffixLen).TrimRight().c_str());
        Result = static_cast<int>(Index);
        break;
      }
    }
  }

  if ((HelpKeyword != nullptr) && (Result >= 0))
  {
    *HelpKeyword = Translation[Result].HelpKeyword;
  }

  return Result;
}
//---------------------------------------------------------------------------
int TSecureShell::TranslateAuthenticationMessage(
  UnicodeString & Message, UnicodeString * HelpKeyword)
{
  static const TPuttyTranslation Translation[] =
  {
    { L"Using username \"%\".", AUTH_TRANSL_USERNAME },
    { L"Using keyboard-interactive authentication.", AUTH_TRANSL_KEYB_INTER }, // not used anymore
    { L"Authenticating with public key \"%\" from agent", AUTH_TRANSL_PUBLIC_KEY_AGENT },
    { L"Authenticating with public key \"%\"", AUTH_TRANSL_PUBLIC_KEY },
    { L"Authenticated using RSA key \"%\" from agent", AUTH_TRANSL_PUBLIC_KEY_AGENT },
    { L"Wrong passphrase", AUTH_TRANSL_WRONG_PASSPHRASE },
    { L"Wrong passphrase.", AUTH_TRANSL_WRONG_PASSPHRASE },
    { L"Access denied", AUTH_TRANSL_ACCESS_DENIED },
    { L"Trying public key authentication.", AUTH_TRANSL_TRY_PUBLIC_KEY },
    { L"Server refused our public key.", AUTH_TRANSL_KEY_REFUSED },
    { L"Server refused our key", AUTH_TRANSL_KEY_REFUSED }
  };

  int Result = TranslatePuttyMessage(Translation, LENOF(Translation), Message, HelpKeyword);

  if ((Result == 2) || (Result == 3) || (Result == 4))
  {
    // GetConfiguration()->GetUsage()->Inc(L"OpenedSessionsPrivateKey2");
    FCollectPrivateKeyUsage = true;
  }

  return Result;
}
//---------------------------------------------------------------------------
void TSecureShell::AddStdError(const UnicodeString & Str)
{
  FStdError += Str;

  intptr_t P = 0;
  UnicodeString Str2 = DeleteChar(Str, L'\r');
  // We send only whole line at once to log, so we have to cache
  // incoming std error data
  FStdErrorTemp += Str2;
  UnicodeString Line;
  // Do we have at least one complete line in std error cache?
  while ((P = FStdErrorTemp.Pos(L"\n")) > 0)
  {
    Line = FStdErrorTemp.SubString(1, P - 1);
    FStdErrorTemp.Delete(1, P);
    AddStdErrorLine(Line);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::AddStdErrorLine(const UnicodeString & Str)
{
  if (FAuthenticating)
  {
    FAuthenticationLog += (FAuthenticationLog.IsEmpty() ? L"" : L"\n") + Str;
  }
  if (!Str.Trim().IsEmpty())
  {
    CaptureOutput(llStdError, Str);
  }
}
//---------------------------------------------------------------------------
const UnicodeString & TSecureShell::GetStdError() const
{
  return FStdError;
}
//---------------------------------------------------------------------------
void TSecureShell::ClearStdError()
{
  // Flush std error cache
  if (!FStdErrorTemp.IsEmpty())
  {
    if (FAuthenticating)
    {
      FAuthenticationLog +=
        (FAuthenticationLog.IsEmpty() ? L"" : L"\n") + FStdErrorTemp;
    }
    CaptureOutput(llStdError, FStdErrorTemp);
    FStdErrorTemp = L"";
  }
  FStdError = L"";
}
//---------------------------------------------------------------------------
void TSecureShell::CaptureOutput(TLogLineType Type,
  const UnicodeString & Line)
{
  if (FOnCaptureOutput != nullptr)
  {
    FOnCaptureOutput(Line, (Type == llStdError));
  }
  FLog->Add(Type, Line);
}
//---------------------------------------------------------------------------
int TSecureShell::TranslateErrorMessage(
  UnicodeString & Message, UnicodeString * HelpKeyword)
{
  static const TPuttyTranslation Translation[] =
  {
    { L"Server unexpectedly closed network connection", UNEXPECTED_CLOSE_ERROR, HELP_UNEXPECTED_CLOSE_ERROR },
    { L"Network error: Connection refused", NET_TRANSL_REFUSED2, HELP_NET_TRANSL_REFUSED },
    { L"Network error: Connection reset by peer", NET_TRANSL_RESET, HELP_NET_TRANSL_RESET },
    { L"Network error: Connection timed out", NET_TRANSL_TIMEOUT2, HELP_NET_TRANSL_TIMEOUT },
    { L"Network error: No route to host", NET_TRANSL_NO_ROUTE2, HELP_NET_TRANSL_NO_ROUTE },
    { L"Network error: Software caused connection abort", NET_TRANSL_CONN_ABORTED, HELP_NET_TRANSL_CONN_ABORTED },
    { L"Host does not exist", NET_TRANSL_HOST_NOT_EXIST2, HELP_NET_TRANSL_HOST_NOT_EXIST },
    { L"Incoming packet was garbled on decryption", NET_TRANSL_PACKET_GARBLED, HELP_NET_TRANSL_PACKET_GARBLED },
  };

  int Index = TranslatePuttyMessage(Translation, LENOF(Translation), Message, HelpKeyword);

  if ((Index == 0) || (Index == 1) || (Index == 2) || (Index == 3))
  {
    FNoConnectionResponse = true;
  }

  Message = ReplaceStr(Message, L"%HOST%", FSessionData->GetHostNameExpanded());

  return Index;
}
//---------------------------------------------------------------------------
void TSecureShell::PuttyFatalError(const UnicodeString & Error)
{
  UnicodeString Error2 = Error;
  UnicodeString HelpKeyword;
  TranslateErrorMessage(Error2, &HelpKeyword);

  FatalError(Error2, HelpKeyword);
}
//---------------------------------------------------------------------------
void TSecureShell::FatalError(const UnicodeString & Error, const UnicodeString & HelpKeyword)
{
  FUI->FatalError(nullptr, Error, HelpKeyword);
}
//---------------------------------------------------------------------------
void TSecureShell::LogEvent(const UnicodeString & Str)
{
  if (FLog->GetLogging())
  {
    FLog->Add(llMessage, Str);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::SocketEventSelect(SOCKET Socket, HANDLE Event, bool Startup)
{
  int Events;

  if (Startup)
  {
    Events = (FD_CONNECT | FD_READ | FD_WRITE | FD_OOB | FD_CLOSE | FD_ACCEPT);
  }
  else
  {
    Events = 0;
  }

  if (GetConfiguration()->GetActualLogProtocol() >= 2)
  {
    LogEvent(FORMAT(L"Selecting events %d for socket %d", static_cast<int>(Events), static_cast<int>(Socket)));
  }

  if (::WSAEventSelect(Socket, (WSAEVENT)Event, Events) == SOCKET_ERROR)
  {
    if (GetConfiguration()->GetActualLogProtocol() >= 2)
    {
      LogEvent(FORMAT(L"Error selecting events %d for socket %d", static_cast<int>(Events), static_cast<int>(Socket)));
    }

    if (Startup)
    {
      FatalError(FMTLOAD(EVENT_SELECT_ERROR, ::WSAGetLastError()));
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::UpdateSocket(SOCKET Value, bool Startup)
{
  if (!FActive && !Startup)
  {
    // no-op
    // Remove the branch eventually:
    // When TCP connection fails, PuTTY does not release the memory allocated for
    // socket. As a simple hack we call sk_tcp_close() in ssh.c to release the memory,
    // until they fix it better. Unfortunately sk_tcp_close calls do_select,
    // so we must filter that out.
  }
  else
  {
    assert(Value);
    assert((FActive && (FSocket == Value)) || (!FActive && Startup));

    // filter our "local proxy" connection, which have no socket
    if (Value != INVALID_SOCKET)
    {
      SocketEventSelect(Value, FSocketEvent, Startup);
    }
    else
    {
      assert(FSessionData->GetProxyMethod() == pmCmd);
    }

    if (Startup)
    {
      FSocket = Value;
      FActive = true;
    }
    else
    {
      FSocket = INVALID_SOCKET;
      Discard();
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::UpdatePortFwdSocket(SOCKET Value, bool Startup)
{
  if (GetConfiguration()->GetActualLogProtocol() >= 2)
  {
    LogEvent(FORMAT(L"Updating forwarding socket %d (%d)", static_cast<int>(Value), static_cast<int>(Startup)));
  }

  SocketEventSelect(Value, FSocketEvent, Startup);

  if (Startup)
  {
    FPortFwdSockets.push_back(Value);
  }
  else
  {
    TSockets::iterator it = FPortFwdSockets.find(Value);
    if (it != FPortFwdSockets.end())
      FPortFwdSockets.erase(it);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::SetActive(bool Value)
{
  if (FActive != Value)
  {
    if (Value)
    {
      Open();
    }
    else
    {
      Close();
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::FreeBackend()
{
  if (FBackendHandle != nullptr)
  {
    FBackend->bfree(FBackendHandle);
    FBackendHandle = nullptr;
  }
}
//---------------------------------------------------------------------------
void TSecureShell::Discard()
{
  bool WasActive = FActive;
  FActive = false;
  FOpened = false;

  if (WasActive)
  {
    FUI->Closed();
  }
}
//---------------------------------------------------------------------------
void TSecureShell::Close()
{
  LogEvent(L"Closing connection.");
  assert(FActive);

  // this is particularly necessary when using local proxy command
  // (e.g. plink), otherwise it hangs in sk_localproxy_close
  SendEOF();

  FreeBackend();

  Discard();
}
//---------------------------------------------------------------------------
void inline TSecureShell::CheckConnection(int Message)
{
  if (!FActive || get_ssh_state_closed(FBackendHandle))
  {
    UnicodeString Str;
    UnicodeString HelpKeyword;

    if (Message >= 0)
    {
      Str = LoadStr(Message);
    }
    else
    {
      Str = LoadStr(NOT_CONNECTED);
      HelpKeyword = HELP_NOT_CONNECTED;
    }

    Str = MainInstructions(Str);

    int ExitCode = get_ssh_exitcode(FBackendHandle);
    if (ExitCode >= 0)
    {
      Str += L" " + FMTLOAD(SSH_EXITCODE, ExitCode);
    }
    FatalError(Str, HelpKeyword);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::PoolForData(WSANETWORKEVENTS & Events, intptr_t & Result)
{
  if (!GetActive())
  {
    // see comment below
    Result = qaRetry;
  }
  else
  {
    try
    {
      if (GetConfiguration()->GetActualLogProtocol() >= 2)
      {
        LogEvent(L"Pooling for data in case they finally arrives");
      }

      // in extreme condition it may happen that send buffer is full, but there
      // will be no data coming and we may not empty the send buffer because we
      // do not process FD_WRITE until we receive any FD_READ
      if (EventSelectLoop(0, false, &Events))
      {
        LogEvent(L"Data has arrived, closing query to user.");
        Result = qaOK;
      }
    }
    catch (...)
    {
      // if we let the exception out, it may popup another message dialog
      // in whole event loop, another call to PoolForData from original dialog
      // would be invoked, leading to an infinite loop.
      // by retrying we hope (that probably fatal) error would repeat in WaitForData.
      // anyway now once no actual work is done in EventSelectLoop,
      // hardly any exception can occur actually
      Result = qaRetry;
    }
  }
}
//---------------------------------------------------------------------------
class TPoolForDataEvent : public TObject
{
NB_DISABLE_COPY(TPoolForDataEvent)
public:
  TPoolForDataEvent(TSecureShell * SecureShell, WSANETWORKEVENTS & Events) :
    FSecureShell(SecureShell),
    FEvents(Events)
  {
  }

  void PoolForData(intptr_t & Result)
  {
    FSecureShell->PoolForData(FEvents, Result);
  }

private:
  TSecureShell * FSecureShell;
  WSANETWORKEVENTS & FEvents;
};
//---------------------------------------------------------------------------
void TSecureShell::WaitForData()
{
  // see winsftp.c
  bool IncomingData;

  do
  {
    if (GetConfiguration()->GetActualLogProtocol() >= 2)
    {
      LogEvent(L"Looking for incoming data");
    }

    IncomingData = EventSelectLoop(FSessionData->GetTimeout() * MSecsPerSec, true, nullptr);
    if (!IncomingData)
    {
      assert(FWaitingForData == 0);
      TAutoNestingCounter NestingCounter(FWaitingForData);

      WSANETWORKEVENTS Events;
      memset(&Events, 0, sizeof(Events));
      TPoolForDataEvent Event(this, Events);

      LogEvent(L"Waiting for data timed out, asking user what to do.");
      uintptr_t Answer = TimeoutPrompt(MAKE_CALLBACK(TPoolForDataEvent::PoolForData, &Event));
      switch (Answer)
      {
        case qaRetry:
          // noop
          break;

        case qaOK:
          // read event was already captured in PoolForData(),
          // make sure we do not try to select it again as it would timeout
          // unless another read event occurs
          IncomingData = true;
          HandleNetworkEvents(FSocket, Events);
          break;

        default:
          FAIL;
          // fallthru

        case qaAbort:
          FatalError(MainInstructions(LoadStr(USER_TERMINATED)));
          break;
      }
    }
  }
  while (!IncomingData);
}
//---------------------------------------------------------------------------
bool TSecureShell::SshFallbackCmd() const
{
  return ssh_fallback_cmd(FBackendHandle) != 0;
}
//---------------------------------------------------------------------------
bool TSecureShell::EnumNetworkEvents(SOCKET Socket, WSANETWORKEVENTS & Events)
{
  if (GetConfiguration()->GetActualLogProtocol() >= 2)
  {
    LogEvent(FORMAT(L"Enumerating network events for socket %d", static_cast<int>(Socket)));
  }

  // see winplink.c
  WSANETWORKEVENTS AEvents;
  if (::WSAEnumNetworkEvents(Socket, nullptr, &AEvents) == 0)
  {
    noise_ultralight(static_cast<uint32_t>(Socket));
    noise_ultralight(AEvents.lNetworkEvents);

    Events.lNetworkEvents |= AEvents.lNetworkEvents;
    for (intptr_t Index = 0; Index < FD_MAX_EVENTS; ++Index)
    {
      if (AEvents.iErrorCode[Index] != 0)
      {
        Events.iErrorCode[Index] = AEvents.iErrorCode[Index];
      }
    }

    if (GetConfiguration()->GetActualLogProtocol() >= 2)
    {
      LogEvent(FORMAT(L"Enumerated %d network events making %d cumulative events for socket %d",
        static_cast<int>(AEvents.lNetworkEvents), static_cast<int>(Events.lNetworkEvents), static_cast<int>(Socket)));
    }
  }
  else
  {
    if (GetConfiguration()->GetActualLogProtocol() >= 2)
    {
      LogEvent(FORMAT(L"Error enumerating network events for socket %d", static_cast<int>(Socket)));
    }
  }

  return
    FLAGSET(Events.lNetworkEvents, FD_READ) ||
    FLAGSET(Events.lNetworkEvents, FD_CLOSE);
}
//---------------------------------------------------------------------------
void TSecureShell::HandleNetworkEvents(SOCKET Socket, WSANETWORKEVENTS & Events)
{
  static const struct { int Bit, Mask; const wchar_t * Desc; } EventTypes[] =
  {
    { FD_READ_BIT, FD_READ, L"read" },
    { FD_WRITE_BIT, FD_WRITE, L"write" },
    { FD_OOB_BIT, FD_OOB, L"oob" },
    { FD_ACCEPT_BIT, FD_ACCEPT, L"accept" },
    { FD_CONNECT_BIT, FD_CONNECT, L"connect" },
    { FD_CLOSE_BIT, FD_CLOSE, L"close" },
  };

  for (uintptr_t Event = 0; Event < LENOF(EventTypes); Event++)
  {
    if (FLAGSET(Events.lNetworkEvents, EventTypes[Event].Mask))
    {
      int Err = Events.iErrorCode[EventTypes[Event].Bit];

      if (GetConfiguration()->GetActualLogProtocol() >= 2)
      {
        LogEvent(FORMAT(L"Handling network %s event on socket %d with error %d",
          EventTypes[Event].Desc, int(Socket), Err));
      }
      LPARAM SelectEvent = WSAMAKESELECTREPLY(EventTypes[Event].Mask, Err);
      if (!select_result(static_cast<WPARAM>(Socket), SelectEvent))
      {
        // note that connection was closed definitely,
        // so "check" is actually not required
        CheckConnection();
      }
    }
  }
}
//---------------------------------------------------------------------------
bool TSecureShell::ProcessNetworkEvents(SOCKET Socket)
{
  WSANETWORKEVENTS Events;
  memset(&Events, 0, sizeof(Events));
  bool Result = EnumNetworkEvents(Socket, Events);
  HandleNetworkEvents(Socket, Events);

  return Result;
}
//---------------------------------------------------------------------------
bool TSecureShell::EventSelectLoop(uintptr_t MSec, bool ReadEventRequired,
  WSANETWORKEVENTS * Events)
{
  CheckConnection();

  bool Result = false;

  do
  {

    if (GetConfiguration()->GetActualLogProtocol() >= 2)
    {
      // LogEvent(L"Looking for network events");
    }
    uintptr_t TicksBefore = GetTickCount();
    int HandleCount;
    // note that this returns all handles, not only the session-related handles
    HANDLE * Handles = handle_get_events(&HandleCount);
    {
      SCOPE_EXIT
      {
        sfree(Handles);
      };
      Handles = sresize(Handles, static_cast<size_t>(HandleCount + 1), HANDLE);
      Handles[HandleCount] = FSocketEvent;
      DWORD Timeout = (DWORD)MSec;
      if (toplevel_callback_pending())
      {
        Timeout = 0;
      }
      uint32_t WaitResult = ::WaitForMultipleObjects(HandleCount + 1, Handles, FALSE, Timeout);
      if (WaitResult < WAIT_OBJECT_0 + HandleCount)
      {
        if (handle_got_event(Handles[WaitResult - WAIT_OBJECT_0]))
        {
          Result = true;
        }
      }
      else if (WaitResult == WAIT_OBJECT_0 + HandleCount)
      {

        if (GetConfiguration()->GetActualLogProtocol() >= 1)
        {
          LogEvent(L"Detected network event");
        }

        if (Events == nullptr)
        {
          if (ProcessNetworkEvents(FSocket))
          {
            Result = true;
          }
        }
        else
        {
          if (EnumNetworkEvents(FSocket, *Events))
          {
            Result = true;
          }
        }

        {
          TSockets::iterator it = FPortFwdSockets.begin();
          while (it != FPortFwdSockets.end())
          {
            ProcessNetworkEvents(*it);
            ++it;
          }
        }
      }
      else if (WaitResult == WAIT_TIMEOUT)
      {

        if (GetConfiguration()->GetActualLogProtocol() >= 2)
        {
          // LogEvent(L"Timeout waiting for network events");
        }

        MSec = 0;
      }
      else
      {

        if (GetConfiguration()->GetActualLogProtocol() >= 2)
        {
          LogEvent(FORMAT(L"Unknown waiting result %d", static_cast<int>(WaitResult)));
        }

        MSec = 0;
      }
    }

    run_toplevel_callbacks();

    uintptr_t TicksAfter = GetTickCount();
    // ticks wraps once in 49.7 days
    if (TicksBefore < TicksAfter)
    {
      uintptr_t Ticks = TicksAfter - TicksBefore;
      if (Ticks > MSec)
      {
        MSec = 0;
      }
      else
      {
        MSec -= Ticks;
      }
    }
  }
  while (ReadEventRequired && (MSec > 0) && !Result);

  return Result;
}
//---------------------------------------------------------------------------
void TSecureShell::Idle(uintptr_t MSec)
{
  noise_regular();

  call_ssh_timer(FBackendHandle);

  // if we are actively waiting for data in WaitForData,
  // do not read here, otherwise we swallow read event and never wake
  if (FWaitingForData <= 0)
  {
    EventSelectLoop(MSec, false, nullptr);
  }
}
//---------------------------------------------------------------------------
void TSecureShell::KeepAlive()
{
  if (FActive && (FWaiting == 0))
  {
    LogEvent(L"Sending null packet to keep session alive.");
    SendSpecial(TS_PING);
  }
  else
  {
    // defer next keepalive attempt
    FLastDataSent = Now();
  }
}
//---------------------------------------------------------------------------
static uint32_t minPacketSize = 0;

uint32_t TSecureShell::MinPacketSize()
{
  if (!FSessionInfoValid)
  {
    UpdateSessionInfo();
  }

  if (FSshVersion == 1)
  {
    return 0;
  }
  else
  {
    if (FMinPacketSize == nullptr)
    {
      FMinPacketSize = &minPacketSize;
    }
    return *FMinPacketSize;
  }
}
//---------------------------------------------------------------------------
uint32_t TSecureShell::MaxPacketSize()
{
  if (!FSessionInfoValid)
  {
    UpdateSessionInfo();
  }

  if (FSshVersion == 1)
  {
    return 0;
  }
  else
  {
    if (FMaxPacketSize == nullptr)
    {
      FMaxPacketSize = ssh2_remmaxpkt(FBackendHandle);
    }
    return *FMaxPacketSize;
  }
}
//---------------------------------------------------------------------------
UnicodeString TSecureShell::FuncToCompression(
  int SshVersion, const void * Compress) const
{
  enum TCompressionType
  {
    ctNone,
    ctZLib
  };
  if (SshVersion == 1)
  {
    return get_ssh1_compressing(FBackendHandle) ? L"ZLib" : L"";
  }
  else
  {
    return reinterpret_cast<ssh_compress *>(const_cast<void *>(Compress)) == &ssh_zlib ? L"ZLib" : L"";
  }
}
//---------------------------------------------------------------------------
TCipher TSecureShell::FuncToSsh1Cipher(const void * Cipher)
{
  const ssh_cipher *CipherFuncs[] =
    {&ssh_3des, &ssh_des, &ssh_blowfish_ssh1};
  const TCipher TCiphers[] = {cip3DES, cipDES, cipBlowfish};
  assert(LENOF(CipherFuncs) == LENOF(TCiphers));
  TCipher Result = cipWarn;

  for (intptr_t Index = 0; Index < static_cast<intptr_t>(LENOF(TCiphers)); ++Index)
  {
    if (static_cast<ssh_cipher *>(const_cast<void *>(Cipher)) == CipherFuncs[Index])
    {
      Result = TCiphers[Index];
    }
  }

  assert(Result != cipWarn);
  return Result;
}
//---------------------------------------------------------------------------
TCipher TSecureShell::FuncToSsh2Cipher(const void * Cipher)
{
  const ssh2_ciphers *CipherFuncs[] =
    {&ssh2_3des, &ssh2_des, &ssh2_aes, &ssh2_blowfish, &ssh2_arcfour};
  const TCipher TCiphers[] = {cip3DES, cipDES, cipAES, cipBlowfish, cipArcfour};
  assert(LENOF(CipherFuncs) == LENOF(TCiphers));
  TCipher Result = cipWarn;

  for (uintptr_t C = 0; C < LENOF(TCiphers); C++)
  {
    for (int F = 0; F < CipherFuncs[C]->nciphers; F++)
    {
      if (reinterpret_cast<ssh2_cipher *>(const_cast<void *>(Cipher)) == CipherFuncs[C]->list[F])
      {
        Result = TCiphers[C];
      }
    }
  }

  assert(Result != cipWarn);
  return Result;
}
//---------------------------------------------------------------------------
//---------------------------------------------------------------------------
UnicodeString TSecureShell::FormatKeyStr(const UnicodeString & KeyStr) const
{
  UnicodeString Result = KeyStr;
  int Index = 1;
  int Digits = 0;
  while (Index <= Result.Length())
  {
    if (IsHex(Result[Index]))
    {
      Digits++;
      if (Digits >= 16)
      {
        Result.Insert(L" ", Index + 1);
        ++Index;
        Digits = 0;
      }
    }
    else
    {
      Digits = 0;
    }
    ++Index;
  }
  return Result;
}
//---------------------------------------------------------------------------
void TSecureShell::VerifyHostKey(const UnicodeString & Host, int Port,
  const UnicodeString & KeyType, const UnicodeString & KeyStr, const UnicodeString & Fingerprint)
{
  UnicodeString Host2 = Host;
  UnicodeString KeyStr2 = KeyStr;
  LogEvent(FORMAT(L"Verifying host key %s %s with fingerprint %s", KeyType.c_str(), FormatKeyStr(KeyStr2).c_str(), Fingerprint.c_str()));

  GotHostKey();

  wchar_t Delimiter = L';';
  assert(KeyStr2.Pos(Delimiter) == 0);

  if (FSessionData->GetTunnel())
  {
    Host2 = FSessionData->GetOrigHostName();
    Port = static_cast<int>(FSessionData->GetOrigPortNumber());
  }

  FSessionInfo.HostKeyFingerprint = Fingerprint;
  UnicodeString NormalizedFingerprint = NormalizeFingerprint(Fingerprint);

  bool Result = false;

  UnicodeString StoredKeys;
  AnsiString AnsiStoredKeys(10240, '\0');

  if (retrieve_host_key(
        Sysutils::W2MB(Host2.c_str(),
             static_cast<UINT>(FSessionData->GetCodePageAsNumber())).c_str(),
        Port,
        Sysutils::W2MB(KeyType.c_str(),
             static_cast<UINT>(FSessionData->GetCodePageAsNumber())).c_str(),
        const_cast<char *>(AnsiStoredKeys.c_str()), (int)AnsiStoredKeys.Length()) == 0)
  {
    StoredKeys = AnsiStoredKeys.c_str();
    UnicodeString Buf = StoredKeys;
    while (!Result && !Buf.IsEmpty())
    {
      UnicodeString StoredKey = CutToChar(Buf, Delimiter, false);
      bool Fingerprint = (StoredKey.SubString(1, 2) != L"0x");
      // its probably a fingerprint (stored by TSessionData::CacheHostKey)
      UnicodeString NormalizedExpectedKey;
      if (Fingerprint)
      {
        NormalizedExpectedKey = NormalizeFingerprint(StoredKey);
      }
      if ((!Fingerprint && (StoredKey == KeyStr2)) ||
          (Fingerprint && (NormalizedExpectedKey == NormalizedFingerprint)))
      {
        LogEvent(L"Host key matches cached key");
        Result = true;
      }
      else
      {
        UnicodeString FormattedKey = Fingerprint ? StoredKey : FormatKeyStr(StoredKey);
        LogEvent(FORMAT(L"Host key does not match cached key %s", FormattedKey.c_str()));
      }
    }
  }

  bool ConfiguredKeyNotMatch = false;

  if (!Result && !FSessionData->GetHostKey().IsEmpty() &&
      (StoredKeys.IsEmpty() || FSessionData->GetOverrideCachedHostKey()))
  {
    UnicodeString Buf = FSessionData->GetHostKey();
    while (!Result && !Buf.IsEmpty())
    {
      UnicodeString ExpectedKey = CutToChar(Buf, Delimiter, false);
      UnicodeString NormalizedExpectedKey = NormalizeFingerprint(ExpectedKey);
      if (ExpectedKey == L"*")
      {
        UnicodeString Message = LoadStr(ANY_HOSTKEY);
        FUI->Information(Message, true);
        FLog->Add(llException, Message);
        Result = true;
      }
      else if (NormalizedExpectedKey == NormalizedFingerprint)
      {
        LogEvent(L"Host key matches configured key");
        Result = true;
      }
      else
      {
        LogEvent(FORMAT(L"Host key does not match configured key %s", ExpectedKey.c_str()));
      }
    }

    if (!Result)
    {
      ConfiguredKeyNotMatch = true;
    }
  }

  if (!Result)
  {
    bool Verified;
    if (ConfiguredKeyNotMatch || GetConfiguration()->GetDisableAcceptingHostKeys())
    {
      Verified = false;
    }
    else
    {
      TClipboardHandler ClipboardHandler;
      ClipboardHandler.Text = Fingerprint;

      bool Unknown = StoredKeys.IsEmpty();

      uintptr_t Answers;
      uintptr_t AliasesCount;
      TQueryButtonAlias Aliases[3];
      Aliases[0].Button = qaRetry;
      Aliases[0].Alias = LoadStr(COPY_KEY_BUTTON);
      Aliases[0].OnClick = MAKE_CALLBACK(TClipboardHandler::Copy, &ClipboardHandler);
      Answers = qaYes | qaCancel | qaRetry;
      AliasesCount = 1;
      if (!Unknown)
      {
        Aliases[1].Button = qaYes;
        Aliases[1].Alias = LoadStr(UPDATE_KEY_BUTTON);
        Aliases[2].Button = qaOK;
        Aliases[2].Alias = LoadStr(ADD_KEY_BUTTON);
        AliasesCount += 2;
        Answers |= qaSkip | qaOK;
      }
      else
      {
        Answers |= qaNo;
      }

      TQueryParams Params;
      Params.NoBatchAnswers = qaYes | qaRetry | qaSkip | qaOK;
      Params.HelpKeyword = (Unknown ? HELP_UNKNOWN_KEY : HELP_DIFFERENT_KEY);
      Params.Aliases = Aliases;
      Params.AliasesCount = AliasesCount;
      uintptr_t R = FUI->QueryUser(
        FMTLOAD((Unknown ? UNKNOWN_KEY3 : DIFFERENT_KEY4), KeyType.c_str(), Fingerprint.c_str()),
        nullptr, Answers, &Params, qtWarning);

      switch (R)
      {
        case qaOK:
          assert(!Unknown);
          KeyStr2 = (StoredKeys + Delimiter + KeyStr2);
          // fall thru
        case qaYes:
          store_host_key(AnsiString(Host2).c_str(), Port, AnsiString(KeyType).c_str(), AnsiString(KeyStr2).c_str());
          Verified = true;
          break;

        case qaCancel:
          Verified = false;
          break;

        default:
          Verified = true;
          break;
      }
    }

    if (!Verified)
    {
      // Configuration->Usage->Inc(L"HostNotVerified");

      UnicodeString Message =
        ConfiguredKeyNotMatch ? FMTLOAD(CONFIGURED_KEY_NOT_MATCH, FSessionData->GetHostKey().c_str()) : LoadStr(KEY_NOT_VERIFIED);
      std::unique_ptr<Exception> E(new Exception(MainInstructions(Message)));
      FUI->FatalError(E.get(), FMTLOAD(HOSTKEY, Fingerprint.c_str()));
    }
  }
}
//---------------------------------------------------------------------------
void TSecureShell::AskAlg(const UnicodeString & AlgType,
  const UnicodeString & AlgName)
{
  UnicodeString Msg;
  if (AlgType == L"key-exchange algorithm")
  {
    Msg = FMTLOAD(KEX_BELOW_TRESHOLD, AlgName.c_str());
  }
  else
  {
    int CipherType = 0;
    if (AlgType == L"cipher")
    {
      CipherType = CIPHER_TYPE_BOTH;
    }
    else if (AlgType == L"client-to-server cipher")
    {
      CipherType = CIPHER_TYPE_CS;
    }
    else if (AlgType == L"server-to-client cipher")
    {
      CipherType = CIPHER_TYPE_SC;
    }
    else
    {
      FAIL;
    }

    Msg = FMTLOAD(CIPHER_BELOW_TRESHOLD, LoadStr(CipherType).c_str(), AlgName.c_str());
  }

  if (FUI->QueryUser(Msg, nullptr, qaYes | qaNo, nullptr, qtWarning) == qaNo)
  {
    Abort();
  }
}
//---------------------------------------------------------------------------
void TSecureShell::DisplayBanner(const UnicodeString & Banner)
{
  FUI->DisplayBanner(Banner);
}
//---------------------------------------------------------------------------
void TSecureShell::OldKeyfileWarning()
{
  // actually never called, see Net.cpp
  FUI->QueryUser(LoadStr(OLD_KEY), nullptr, qaOK, nullptr, qtWarning);
}
//---------------------------------------------------------------------------
bool TSecureShell::GetStoredCredentialsTried() const
{
  return FStoredPasswordTried || FStoredPasswordTriedForKI || FStoredPassphraseTried;
}
//---------------------------------------------------------------------------
bool TSecureShell::GetReady() const
{
  return FOpened && (FWaiting == 0);
}
//---------------------------------------------------------------------------
void TSecureShell::CollectUsage()
{
  if (FCollectPrivateKeyUsage)
  {
//    Configuration->Usage->Inc(L"OpenedSessionsPrivateKey2");
  }

  if (FSshVersion == 1)
  {
    // Configuration->Usage->Inc(L"OpenedSessionsSSH1");
  }

  if (GetSshImplementation() == sshiOpenSSH)
  {
    // Configuration->Usage->Inc(L"OpenedSessionsSSHOpenSSH");
  }
  else if (GetSshImplementation() == sshiProFTPD)
  {
    // Configuration->Usage->Inc(L"OpenedSessionsSSHProFTPD");
  }
  else if (GetSshImplementation() == sshiBitvise)
  {
    // Configuration->Usage->Inc(L"OpenedSessionsSSHBitvise");
  }
  else
  {
    // Configuration->Usage->Inc(L"OpenedSessionsSSHOther");
  }
}
//------------------------------------------------------------------------------
NB_IMPLEMENT_CLASS(TSecureShell, NB_GET_CLASS_INFO(TObject), nullptr);
//------------------------------------------------------------------------------
