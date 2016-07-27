// Copyright 2015 Dolphin Emulator Project
// Licensed under GPLv2+
// Refer to the license.txt file included.

#include <algorithm>
#include <cstddef>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/clipbrd.h>
#include <wx/colour.h>
#include <wx/dialog.h>
#include <wx/frame.h>
#include <wx/listbox.h>
#include <wx/msgdlg.h>
#include <wx/notebook.h>
#include <wx/panel.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>
#include <wx/string.h>
#include <wx/textctrl.h>

#include "Common/CommonPaths.h"
#include "Common/CommonTypes.h"
#include "Common/FifoQueue.h"
#include "Common/FileUtil.h"
#include "Common/IniFile.h"

#include "Core/ConfigManager.h"
#include "Core/HW/EXI_Device.h"
#include "Core/NetPlayClient.h"
#include "Core/NetPlayProto.h"
#include "Core/NetPlayServer.h"

#include "DiscIO/Enums.h"

#include "DolphinWX/Frame.h"
#include "DolphinWX/GameListCtrl.h"
#include "DolphinWX/ISOFile.h"
#include "DolphinWX/Main.h"
#include "DolphinWX/NetPlay/ChangeGameDialog.h"
#include "DolphinWX/NetPlay/NetWindow.h"
#include "DolphinWX/NetPlay/PadMapDialog.h"
#include "DolphinWX/WxUtils.h"
#include "MD5Dialog.h"

#include "VideoCommon/OnScreenDisplay.h"
#include "VideoCommon/VideoConfig.h"

NetPlayServer* NetPlayDialog::netplay_server = nullptr;
NetPlayClient* NetPlayDialog::netplay_client = nullptr;
NetPlayDialog* NetPlayDialog::npd = nullptr;

static std::string BuildGameName(const GameListItem& game)
{
	// Lang needs to be consistent
	const DiscIO::Language lang = DiscIO::Language::LANGUAGE_ENGLISH;
	std::vector<std::string> info;
	if (!game.GetUniqueID().empty())
		info.push_back(game.GetUniqueID());
	if (game.GetRevision() != 0)
	{
		std::string rev_str = "Revision ";
		info.push_back(rev_str + std::to_string((long long)game.GetRevision()));
	}

	std::string name(game.GetName(lang));
	if (name.empty())
		name = game.GetName();

	int disc_number = game.GetDiscNumber() + 1;

	std::string lower_name = name;
	std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
	if (disc_number > 1 &&
		lower_name.find(std::string(wxString::Format("disc %i", disc_number))) == std::string::npos &&
		lower_name.find(std::string(wxString::Format("disc%i", disc_number))) == std::string::npos)
	{
		std::string disc_text = "Disc ";
		info.push_back(disc_text + std::to_string(disc_number));
	}
	if (info.empty())
		return name;
	std::ostringstream ss;
	std::copy(info.begin(), info.end() - 1, std::ostream_iterator<std::string>(ss, ", "));
	ss << info.back();
	return name + " (" + ss.str() + ")";
}

void NetPlayDialog::FillWithGameNames(wxListBox* game_lbox, const CGameListCtrl& game_list)
{
	for (u32 i = 0; auto game = game_list.GetISO(i); ++i)
		game_lbox->Append(StrToWxStr(BuildGameName(*game)));
}

NetPlayDialog::NetPlayDialog(wxWindow* const parent, const CGameListCtrl* const game_list,
	const std::string& game, const bool is_hosting)
	: wxFrame(parent, wxID_ANY, _("Dolphin NetPlay")), m_selected_game(game), m_start_btn(nullptr),
	m_host_label(nullptr), m_host_type_choice(nullptr), m_host_copy_btn(nullptr),
	m_host_copy_btn_is_retry(false), m_is_hosting(is_hosting), m_game_list(game_list)
{
	Bind(wxEVT_THREAD, &NetPlayDialog::OnThread, this);

	wxPanel* const panel = new wxPanel(this);

	wxBoxSizer* const top_szr = new wxBoxSizer(wxHORIZONTAL);

	m_game_btn = new wxButton(panel, wxID_ANY, StrToWxStr(m_selected_game).Prepend(_(" Game : ")),
		wxDefaultPosition, wxDefaultSize, wxBU_LEFT);

	if (m_is_hosting)
		m_game_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnChangeGame, this);
	else
		m_game_btn->Disable();

	top_szr->Add(m_game_btn, 1, wxALL | wxEXPAND);

	if (m_is_hosting)
	{
		m_MD5_choice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxSize(150, -1));
		m_MD5_choice->Bind(wxEVT_CHOICE, &NetPlayDialog::OnMD5ComputeRequested, this);
		m_MD5_choice->Append(_("MD5 check..."));
		m_MD5_choice->Append(_("Current game"));
		m_MD5_choice->Append(_("Other game"));
		m_MD5_choice->Append(_("SD card"));
		m_MD5_choice->SetSelection(0);

		top_szr->Add(m_MD5_choice, 0, wxALL);
	}

	// middle crap

	// chat
	m_chat_text = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize,
		wxTE_READONLY | wxTE_MULTILINE);

	m_chat_msg_text = new wxTextCtrl(panel, wxID_ANY, wxEmptyString, wxDefaultPosition,
		wxSize(-1, 25), wxTE_PROCESS_ENTER);
	m_chat_msg_text->Bind(wxEVT_TEXT_ENTER, &NetPlayDialog::OnChat, this);
	m_chat_msg_text->SetMaxLength(2000);

	wxButton* const chat_msg_btn =
		new wxButton(panel, wxID_ANY, _("Send"), wxDefaultPosition, wxSize(-1, 26));
	chat_msg_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnChat, this);

	wxBoxSizer* const chat_msg_szr = new wxBoxSizer(wxHORIZONTAL);
	chat_msg_szr->Add(m_chat_msg_text, 1);
	chat_msg_szr->Add(chat_msg_btn, 0);

	wxStaticBoxSizer* const chat_szr = new wxStaticBoxSizer(wxVERTICAL, panel, _("Chat"));
	chat_szr->Add(m_chat_text, 1, wxEXPAND);
	chat_szr->Add(chat_msg_szr, 0, wxEXPAND | wxTOP, 5);

	m_player_lbox = new wxListBox(panel, wxID_ANY, wxDefaultPosition, wxSize(256, -1));

	wxStaticBoxSizer* const player_szr = new wxStaticBoxSizer(wxVERTICAL, panel, _("Players"));

	// player list
	if (m_is_hosting && g_TraversalClient)
	{
		wxBoxSizer* const host_szr = new wxBoxSizer(wxHORIZONTAL);
		m_host_type_choice = new wxChoice(panel, wxID_ANY, wxDefaultPosition, wxSize(76, -1));
		m_host_type_choice->Bind(wxEVT_CHOICE, &NetPlayDialog::OnChoice, this);
		m_host_type_choice->Append(_("Room ID:"));
		host_szr->Add(m_host_type_choice);

		m_host_label = new wxStaticText(panel, wxID_ANY, "555.555.555.555:55555", wxDefaultPosition,
			wxDefaultSize, wxST_NO_AUTORESIZE | wxALIGN_LEFT);
		// Update() should fix this immediately.
		m_host_label->SetLabel("");
		host_szr->Add(m_host_label, 1, wxLEFT | wxCENTER, 5);

		m_host_copy_btn = new wxButton(panel, wxID_ANY, _("Copy"));
		m_host_copy_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnCopyIP, this);
		m_host_copy_btn->Disable();
		host_szr->Add(m_host_copy_btn, 0, wxLEFT | wxCENTER, 5);
		player_szr->Add(host_szr, 0, wxEXPAND | wxBOTTOM, 5);
		m_host_type_choice->Select(0);

		UpdateHostLabel();
	}

	player_szr->Add(m_player_lbox, 1, wxEXPAND);

	if (m_is_hosting)
	{
		m_player_lbox->Bind(wxEVT_LISTBOX, &NetPlayDialog::OnPlayerSelect, this);
		m_kick_btn = new wxButton(panel, wxID_ANY, _("Kick Player"));
		m_kick_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnKick, this);
		player_szr->Add(m_kick_btn, 0, wxEXPAND | wxTOP, 5);
		m_kick_btn->Disable();

		m_player_config_btn = new wxButton(panel, wxID_ANY, _("Assign Controller Ports"));
		m_player_config_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnAssignPads, this);
		player_szr->Add(m_player_config_btn, 0, wxEXPAND | wxTOP, 5);
	}

	wxBoxSizer* const mid_szr = new wxBoxSizer(wxHORIZONTAL);
	mid_szr->Add(chat_szr, 1, wxEXPAND | wxRIGHT, 5);
	mid_szr->Add(player_szr, 0, wxEXPAND);

	// bottom crap
	wxButton* const quit_btn = new wxButton(panel, wxID_ANY, _("Quit Netplay"));
	quit_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnQuit, this);

	wxBoxSizer* const bottom_szr = new wxBoxSizer(wxHORIZONTAL);
	if (is_hosting)
	{
		m_start_btn = new wxButton(panel, wxID_ANY, _("Start"));
		m_start_btn->Bind(wxEVT_BUTTON, &NetPlayDialog::OnStart, this);
		bottom_szr->Add(m_start_btn);

		bottom_szr->Add(new wxStaticText(panel, wxID_ANY, _("Buffer:")), 0, wxLEFT | wxCENTER, 5);
		wxSpinCtrl* const padbuf_spin =
			new wxSpinCtrl(panel, wxID_ANY, std::to_string(INITIAL_PAD_BUFFER_SIZE), wxDefaultPosition,
				wxSize(64, -1), wxSP_ARROW_KEYS, 0, 200, INITIAL_PAD_BUFFER_SIZE);
		padbuf_spin->Bind(wxEVT_SPINCTRL, &NetPlayDialog::OnAdjustBuffer, this);
		bottom_szr->AddSpacer(3);
		bottom_szr->Add(padbuf_spin, 0, wxCENTER);
		bottom_szr->AddSpacer(5);
		m_memcard_write = new wxCheckBox(panel, wxID_ANY, _("Write to memcards/SD"));
		bottom_szr->Add(m_memcard_write, 0, wxCENTER);
	}

	bottom_szr->AddSpacer(5);
	m_record_chkbox = new wxCheckBox(panel, wxID_ANY, _("Record inputs"));
	bottom_szr->Add(m_record_chkbox, 0, wxCENTER);

	bottom_szr->AddStretchSpacer(1);
	bottom_szr->Add(quit_btn);

	// main sizer
	wxBoxSizer* const main_szr = new wxBoxSizer(wxVERTICAL);
	main_szr->Add(top_szr, 0, wxEXPAND | wxALL, 5);
	main_szr->Add(mid_szr, 1, wxEXPAND | wxLEFT | wxRIGHT, 5);
	main_szr->Add(bottom_szr, 0, wxEXPAND | wxALL, 5);

	panel->SetSizerAndFit(main_szr);

	main_szr->SetSizeHints(this);
	SetSize(768, 768 - 128);

	Center();
}

NetPlayDialog::~NetPlayDialog()
{
	if (netplay_client)
	{
		delete netplay_client;
		netplay_client = nullptr;
	}
	if (netplay_server)
	{
		delete netplay_server;
		netplay_server = nullptr;
	}
	npd = nullptr;
}

void NetPlayDialog::OnChat(wxCommandEvent&)
{
	std::string text = WxStrToStr(m_chat_msg_text->GetValue());

	if (!text.empty())
	{
		netplay_client->SendChatMessage(text);
		m_chat_msg_text->Clear();
		AddChatMessage(ChatMessageType::UserOut, text);
	}
}

void NetPlayDialog::GetNetSettings(NetSettings& settings)
{
	SConfig& instance = SConfig::GetInstance();
	settings.m_CPUthread = instance.bCPUThread;
	settings.m_CPUcore = instance.iCPUCore;
	settings.m_EnableCheats = instance.bEnableCheats;
	settings.m_SelectedLanguage = instance.SelectedLanguage;
	settings.m_OverrideGCLanguage = instance.bOverrideGCLanguage;
	settings.m_ProgressiveScan = instance.bProgressive;
	settings.m_PAL60 = instance.bPAL60;
	settings.m_DSPHLE = instance.bDSPHLE;
	settings.m_DSPEnableJIT = instance.m_DSPEnableJIT;
	settings.m_WriteToMemcard = m_memcard_write->GetValue();
	settings.m_OCEnable = instance.m_OCEnable;
	settings.m_OCFactor = instance.m_OCFactor;
	settings.m_EXIDevice[0] = instance.m_EXIDevice[0];
	settings.m_EXIDevice[1] = instance.m_EXIDevice[1];
}

std::string NetPlayDialog::FindGame(const std::string& target_game)
{
	// find path for selected game, sloppy..
	for (u32 i = 0; auto game = m_game_list->GetISO(i); ++i)
		if (target_game == BuildGameName(*game))
			return game->GetFileName();

	return "";
}

std::string NetPlayDialog::FindCurrentGame()
{
	return FindGame(m_selected_game);
}

void NetPlayDialog::OnStart(wxCommandEvent&)
{
	bool should_start = true;
	if (!netplay_client->DoAllPlayersHaveGame())
	{
		should_start = wxMessageBox(_("Not all players have the game. Do you really want to start?"),
			_("Warning"), wxYES_NO) == wxYES;
	}

	if (!should_start)
		return;

	NetSettings settings;
	GetNetSettings(settings);
	netplay_server->SetNetSettings(settings);
	netplay_server->StartGame();
}

void NetPlayDialog::BootGame(const std::string& filename)
{
	main_frame->BootGame(filename);
}

void NetPlayDialog::StopGame()
{
	main_frame->DoStop();
}

// NetPlayUI methods called from ---NETPLAY--- thread
void NetPlayDialog::Update()
{
	wxThreadEvent evt(wxEVT_THREAD, 1);
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::AppendChat(const std::string& msg)
{
	chat_msgs.Push(msg);
	// silly
	Update();
}

void NetPlayDialog::OnMsgChangeGame(const std::string& filename)
{
	wxThreadEvent* evt = new wxThreadEvent(wxEVT_THREAD, NP_GUI_EVT_CHANGE_GAME);
	evt->SetString(StrToWxStr(filename));
	GetEventHandler()->QueueEvent(evt);
}

void NetPlayDialog::OnMsgStartGame()
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_START_GAME);
	GetEventHandler()->AddPendingEvent(evt);
	if (m_is_hosting)
	{
		m_start_btn->Disable();
		m_memcard_write->Disable();
		m_game_btn->Disable();
		m_player_config_btn->Disable();
	}

	m_record_chkbox->Disable();
}

void NetPlayDialog::OnMsgStopGame()
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_STOP_GAME);
	GetEventHandler()->AddPendingEvent(evt);
	if (m_is_hosting)
	{
		m_start_btn->Enable();
		m_memcard_write->Enable();
		m_game_btn->Enable();
		m_player_config_btn->Enable();
	}
	m_record_chkbox->Enable();
}

void NetPlayDialog::OnAdjustBuffer(wxCommandEvent& event)
{
	const int val = ((wxSpinCtrl*)event.GetEventObject())->GetValue();
	netplay_server->AdjustPadBufferSize(val);
}

void NetPlayDialog::OnPadBufferChanged(u32 buffer)
{
	m_pad_buffer = buffer;
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_PAD_BUFFER_CHANGE);
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::OnDesync(u32 frame, const std::string& player)
{
	m_desync_frame = frame;
	m_desync_player = player;
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_DESYNC);
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::OnConnectionLost()
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_CONNECTION_LOST);
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::OnTraversalError(int error)
{
	switch (error)
	{
	case TraversalClient::BadHost:
		PanicAlertT("Couldn't look up central server");
		break;
	case TraversalClient::VersionTooOld:
		PanicAlertT("Dolphin is too old for traversal server");
		break;
	case TraversalClient::ServerForgotAboutUs:
	case TraversalClient::SocketSendError:
	case TraversalClient::ResendTimeout:
		wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_TRAVERSAL_CONNECTION_ERROR);
		GetEventHandler()->AddPendingEvent(evt);
		break;
	}
}

void NetPlayDialog::OnQuit(wxCommandEvent&)
{
	Destroy();
}

// update gui
void NetPlayDialog::OnThread(wxThreadEvent& event)
{
	if (m_is_hosting && m_host_label && g_TraversalClient)
	{
		UpdateHostLabel();
	}

	// player list
	m_playerids.clear();
	std::string tmps;
	netplay_client->GetPlayerList(tmps, m_playerids);

	wxString selection;
	if (m_player_lbox->GetSelection() != wxNOT_FOUND)
		selection = m_player_lbox->GetString(m_player_lbox->GetSelection());

	m_player_lbox->Clear();
	std::istringstream ss(tmps);
	while (std::getline(ss, tmps))
		m_player_lbox->Append(StrToWxStr(tmps));

	// remove ping from selection string, in case it has changed
	selection.erase(selection.rfind('|') + 1);

	if (!selection.empty())
	{
		for (unsigned int i = 0; i < m_player_lbox->GetCount(); ++i)
		{
			if (selection == m_player_lbox->GetString(i).substr(0, selection.length()))
			{
				m_player_lbox->SetSelection(i);
				break;
			}
		}
	}

	// flash window in taskbar when someone joins if window isn't active
	static u8 numPlayers = 1;
	if (netplay_server != nullptr && numPlayers < m_playerids.size() && !HasFocus())
	{
		RequestUserAttention();
	}
	numPlayers = m_playerids.size();

	switch (event.GetId())
	{
	case NP_GUI_EVT_CHANGE_GAME:
		// update selected game :/
	{
		m_selected_game = WxStrToStr(event.GetString());

		wxString button_label = event.GetString();
		m_game_btn->SetLabel(button_label.Prepend(_(" Game : ")));
	}
	break;
	case NP_GUI_EVT_START_GAME:
		// client start game :/
	{
		netplay_client->StartGame(FindCurrentGame());
		std::string msg = "Starting game";
		AddChatMessage(ChatMessageType::Info, msg);
	}
	break;
	case NP_GUI_EVT_STOP_GAME:
		// client stop game
	{
		std::string msg = "Stopping game";
		AddChatMessage(ChatMessageType::Info, msg);
	}
	break;
	case NP_GUI_EVT_DISPLAY_MD5_DIALOG:
	{
		m_MD5_dialog = new MD5Dialog(this, netplay_server, netplay_client->GetPlayers(),
			event.GetString().ToStdString());
		m_MD5_dialog->Show();
	}
	break;
	case NP_GUI_EVT_MD5_PROGRESS:
	{
		if (m_MD5_dialog == nullptr || m_MD5_dialog->IsBeingDeleted())
			break;

		std::pair<int, int> payload = event.GetPayload<std::pair<int, int>>();
		m_MD5_dialog->SetProgress(payload.first, payload.second);
	}
	break;
	case NP_GUI_EVT_MD5_RESULT:
	{
		if (m_MD5_dialog == nullptr || m_MD5_dialog->IsBeingDeleted())
			break;

		std::pair<int, std::string> payload = event.GetPayload<std::pair<int, std::string>>();
		m_MD5_dialog->SetResult(payload.first, payload.second);
	}
	break;
	case NP_GUI_EVT_PAD_BUFFER_CHANGE:
	{
		std::string msg = StringFromFormat("Pad buffer: %d", m_pad_buffer);

		if (g_ActiveConfig.bShowNetPlayMessages)
		{
			OSD::AddTypedMessage(OSD::MessageType::NetPlayBuffer, msg, OSD::Duration::NORMAL);
		}

		AddChatMessage(ChatMessageType::Info, msg);
	}
	break;
	case NP_GUI_EVT_DESYNC:
	{
		std::string msg = "Possible desync detected from player " + m_desync_player + " on frame " +
			std::to_string(m_desync_frame);

		AddChatMessage(ChatMessageType::Error, msg);

		if (g_ActiveConfig.bShowNetPlayMessages)
		{
			OSD::AddMessage(msg, OSD::Duration::VERY_LONG, OSD::Color::RED);
		}
	}
	break;
	case NP_GUI_EVT_CONNECTION_LOST:
	{
		std::string msg = "Lost connection to server";
		AddChatMessage(ChatMessageType::Error, msg);
	}
	break;
	case NP_GUI_EVT_TRAVERSAL_CONNECTION_ERROR:
	{
		std::string msg = "Traversal server connection error";
		AddChatMessage(ChatMessageType::Error, msg);
	}
	}

	// chat messages
	while (chat_msgs.Size())
	{
		std::string s;
		chat_msgs.Pop(s);
		AddChatMessage(ChatMessageType::UserIn, s);

		if (g_ActiveConfig.bShowNetPlayMessages)
		{
			OSD::AddMessage(s, OSD::Duration::NORMAL, OSD::Color::GREEN);
		}
	}
}

void NetPlayDialog::OnChangeGame(wxCommandEvent&)
{
	ChangeGameDialog change_game_dialog(this, m_game_list);
	change_game_dialog.ShowModal();

	wxString game_name = change_game_dialog.GetChosenGameName();
	if (game_name.empty())
		return;

	m_selected_game = WxStrToStr(game_name);
	netplay_server->ChangeGame(m_selected_game);
	m_game_btn->SetLabel(game_name.Prepend(_(" Game : ")));
}

void NetPlayDialog::OnMD5ComputeRequested(wxCommandEvent&)
{
	MD5Target selection = static_cast<MD5Target>(m_MD5_choice->GetSelection());
	std::string file_identifier;
	ChangeGameDialog change_game_dialog(this, m_game_list);

	m_MD5_choice->SetSelection(0);

	switch (selection)
	{
	case MD5Target::CurrentGame:
		file_identifier = m_selected_game;
		break;

	case MD5Target::OtherGame:
		change_game_dialog.ShowModal();

		file_identifier = WxStrToStr(change_game_dialog.GetChosenGameName());
		if (file_identifier.empty())
			return;
		break;

	case MD5Target::SdCard:
		file_identifier = WII_SDCARD;
		break;

	default:
		return;
	}

	netplay_server->ComputeMD5(file_identifier);
}

void NetPlayDialog::ShowMD5Dialog(const std::string& file_identifier)
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_DISPLAY_MD5_DIALOG);
	evt.SetString(file_identifier);
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::SetMD5Progress(int pid, int progress)
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_MD5_PROGRESS);
	evt.SetPayload(std::pair<int, int>(pid, progress));
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::SetMD5Result(int pid, const std::string& result)
{
	wxThreadEvent evt(wxEVT_THREAD, NP_GUI_EVT_MD5_RESULT);
	evt.SetPayload(std::pair<int, std::string>(pid, result));
	GetEventHandler()->AddPendingEvent(evt);
}

void NetPlayDialog::AbortMD5()
{
	if (m_MD5_dialog != nullptr && !m_MD5_dialog->IsBeingDeleted())
		m_MD5_dialog->Destroy();
}

void NetPlayDialog::OnAssignPads(wxCommandEvent&)
{
	PadMapDialog pmd(this, netplay_server, netplay_client);
	pmd.ShowModal();

	netplay_server->SetPadMapping(pmd.GetModifiedPadMappings());
	netplay_server->SetWiimoteMapping(pmd.GetModifiedWiimoteMappings());
}

void NetPlayDialog::OnKick(wxCommandEvent&)
{
	wxString selection = m_player_lbox->GetStringSelection();
	unsigned long player = 0;
	selection.substr(selection.rfind('[') + 1, selection.rfind(']')).ToULong(&player);

	netplay_server->KickPlayer((u8)player);

	m_player_lbox->SetSelection(wxNOT_FOUND);
	wxCommandEvent event;
	OnPlayerSelect(event);
}

void NetPlayDialog::OnPlayerSelect(wxCommandEvent&)
{
	m_kick_btn->Enable(m_player_lbox->GetSelection() > 0);
}

bool NetPlayDialog::IsRecording()
{
	return m_record_chkbox->GetValue();
}

void NetPlayDialog::OnCopyIP(wxCommandEvent&)
{
	if (m_host_copy_btn_is_retry)
	{
		g_TraversalClient->ReconnectToServer();
		Update();
	}
	else
	{
		if (wxTheClipboard->Open())
		{
			wxTheClipboard->SetData(new wxTextDataObject(m_host_label->GetLabel()));
			wxTheClipboard->Close();
		}
	}
}

void NetPlayDialog::OnChoice(wxCommandEvent& event)
{
	UpdateHostLabel();
}

void NetPlayDialog::UpdateHostLabel()
{
	wxString label = _(" (internal IP)");
	auto DeLabel = [=](wxString str) {
		if (str == _("Localhost"))
			return std::string("!local!");
		return WxStrToStr(str.Left(str.Len() - label.Len()));
	};
	auto EnLabel = [=](std::string str) -> wxString {
		if (str == "!local!")
			return _("Localhost");
		return StrToWxStr(str) + label;
	};
	int sel = m_host_type_choice->GetSelection();
	if (sel == 0)
	{
		// the traversal ID
		switch (g_TraversalClient->m_State)
		{
		case TraversalClient::Connecting:
			m_host_label->SetForegroundColour(*wxLIGHT_GREY);
			m_host_label->SetLabel("...");
			m_host_copy_btn->SetLabel(_("Copy"));
			m_host_copy_btn->Disable();
			break;
		case TraversalClient::Connected:
			m_host_label->SetForegroundColour(*wxBLACK);
			m_host_label->SetLabel(
				wxString(g_TraversalClient->m_HostId.data(), g_TraversalClient->m_HostId.size()));
			m_host_copy_btn->SetLabel(_("Copy"));
			m_host_copy_btn->Enable();
			m_host_copy_btn_is_retry = false;
			break;
		case TraversalClient::Failure:
			m_host_label->SetForegroundColour(*wxBLACK);
			m_host_label->SetLabel("...");
			m_host_copy_btn->SetLabel(_("Retry"));
			m_host_copy_btn->Enable();
			m_host_copy_btn_is_retry = true;
			break;
		}
	}
	else if (sel != wxNOT_FOUND)  // wxNOT_FOUND shouldn't generally happen
	{
		m_host_label->SetForegroundColour(*wxBLACK);
		m_host_label->SetLabel(
			netplay_server->GetInterfaceHost(DeLabel(m_host_type_choice->GetString(sel))));
		m_host_copy_btn->SetLabel(_("Copy"));
		m_host_copy_btn->Enable();
		m_host_copy_btn_is_retry = false;
	}

	auto set = netplay_server->GetInterfaceSet();
	for (const std::string& iface : set)
	{
		wxString wxIface = EnLabel(iface);
		if (m_host_type_choice->FindString(wxIface) == wxNOT_FOUND)
			m_host_type_choice->Append(wxIface);
	}
	for (unsigned i = 1, count = m_host_type_choice->GetCount(); i != count; i++)
	{
		if (set.find(DeLabel(m_host_type_choice->GetString(i))) == set.end())
		{
			m_host_type_choice->Delete(i);
			i--;
			count--;
		}
	}
}

void NetPlayDialog::AddChatMessage(ChatMessageType type, const std::string& msg)
{
	wxColour colour = *wxBLACK;
	std::string printed_msg = msg;

	switch (type)
	{
	case ChatMessageType::Info:
		colour = wxColour(0, 150, 150);  // cyan
		break;

	case ChatMessageType::Error:
		colour = *wxRED;
		break;

	case ChatMessageType::UserIn:
		colour = wxColour(0, 150, 0);  // green
		printed_msg = "▶ " + msg;
		break;

	case ChatMessageType::UserOut:
		colour = wxColour(100, 100, 100);  // grey
		printed_msg = "◀ " + msg;
		break;
	}

	if (type == ChatMessageType::Info || type == ChatMessageType::Error)
		printed_msg = "― " + msg + " ―";

	m_chat_text->SetDefaultStyle(wxTextAttr(colour));
	m_chat_text->AppendText(StrToWxStr(printed_msg + "\n"));
}
