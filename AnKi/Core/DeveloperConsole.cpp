// Copyright (C) 2009-2022, Panagiotis Christopoulos Charitos and contributors.
// All rights reserved.
// Code licensed under the BSD License.
// http://www.anki3d.org/LICENSE

#include <AnKi/Core/DeveloperConsole.h>

namespace anki {

DeveloperConsole::~DeveloperConsole()
{
	LoggerSingleton::get().removeMessageHandler(this, loggerCallback);

	while(!m_logItems.isEmpty())
	{
		LogItem* item = &m_logItems.getFront();
		m_logItems.popFront();
		item->m_threadName.destroy(getMemoryPool());
		item->m_msg.destroy(getMemoryPool());
		deleteInstance(getMemoryPool(), item);
	}
}

Error DeveloperConsole::init(ScriptManager* scriptManager)
{
	zeroMemory(m_inputText);

	ANKI_CHECK(m_manager->newInstance(m_font, "EngineAssets/UbuntuMonoRegular.ttf", Array<U32, 1>{16}));

	// Add a new callback to the logger
	LoggerSingleton::get().addMessageHandler(this, loggerCallback);

	ANKI_CHECK(m_scriptEnv.init(scriptManager));

	return Error::kNone;
}

void DeveloperConsole::build(CanvasPtr ctx)
{
	const Vec4 oldWindowColor = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg].w = 0.3f;
	ctx->pushFont(m_font, 16);

	ImGui::Begin("Console", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoTitleBar);

	ImGui::SetWindowPos(Vec2(0.0f, 0.0f));
	ImGui::SetWindowSize(Vec2(F32(ctx->getWidth()), F32(ctx->getHeight()) * (2.0f / 3.0f)));

	// Push the items
	const F32 footerHeightToPreserve = ImGui::GetStyle().ItemSpacing.y + ImGui::GetFrameHeightWithSpacing();
	ImGui::BeginChild("ScrollingRegion", Vec2(0, -footerHeightToPreserve), false,
					  ImGuiWindowFlags_HorizontalScrollbar); // Leave room for 1 separator + 1 InputText

	ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, Vec2(4.0f, 1.0f)); // Tighten spacing

	for(const LogItem& item : m_logItems)
	{
		switch(item.m_type)
		{
		case LoggerMessageType::kNormal:
			ImGui::PushStyleColor(ImGuiCol_Text, Vec4(0.0f, 1.0f, 0.0f, 1.0f));
			break;
		case LoggerMessageType::kError:
		case LoggerMessageType::kFatal:
			ImGui::PushStyleColor(ImGuiCol_Text, Vec4(1.0f, 0.0f, 0.0f, 1.0f));
			break;
		case LoggerMessageType::kWarning:
			ImGui::PushStyleColor(ImGuiCol_Text, Vec4(0.9f, 0.6f, 0.14f, 1.0f));
			break;
		default:
			ANKI_ASSERT(0);
		}

		constexpr Array<const Char*, U(LoggerMessageType::kCount)> kMsgText = {"I", "E", "W", "F"};
		ImGui::TextWrapped("[%s][%s] %s [%s:%d][%s][%s]", kMsgText[item.m_type],
						   (item.m_subsystem) ? item.m_subsystem : "N/A ", item.m_msg.cstr(), item.m_file, item.m_line,
						   item.m_func, item.m_threadName.cstr());

		ImGui::PopStyleColor();
	}

	const U32 timestamp = m_logItemsTimestamp.getNonAtomically();
	const Bool scrollToLast = m_logItemsTimestampConsumed < timestamp;

	if(scrollToLast)
	{
		ImGui::SetScrollHereY(1.0f);
		++m_logItemsTimestampConsumed;
	}
	ImGui::PopStyleVar();
	ImGui::EndChild();

	// Commands
	ImGui::Separator();
	ImGui::PushItemWidth(-1.0f); // Use the whole size
	if(ImGui::InputText("##noname", &m_inputText[0], m_inputText.getSizeInBytes(), ImGuiInputTextFlags_EnterReturnsTrue,
						nullptr, nullptr))
	{
		const Error err = m_scriptEnv.evalString(&m_inputText[0]);
		if(!err)
		{
			ANKI_CORE_LOGI("Script ran without errors");
		}
		m_inputText[0] = '\0';
	}
	ImGui::PopItemWidth();

	ImGui::SetKeyboardFocusHere(-1); // Auto focus previous widget

	ImGui::End();
	ImGui::GetStyle().Colors[ImGuiCol_WindowBg] = oldWindowColor;
	ctx->popFont();
}

void DeveloperConsole::newLogItem(const LoggerMessageInfo& inf)
{
	LogItem* newLogItem;

	// Pop first
	if(m_logItemCount + 1 > kMaxLogItems)
	{
		LogItem* first = &m_logItems.getFront();
		m_logItems.popFront();

		first->m_msg.destroy(getMemoryPool());
		first->m_threadName.destroy(getMemoryPool());

		// Re-use the log item
		newLogItem = first;
		--m_logItemCount;
	}
	else
	{
		newLogItem = newInstance<LogItem>(getMemoryPool());
	}

	// Create the new item
	newLogItem->m_file = inf.m_file;
	newLogItem->m_func = inf.m_func;
	newLogItem->m_subsystem = inf.m_subsystem;
	newLogItem->m_threadName.create(getMemoryPool(), inf.m_threadName);
	newLogItem->m_msg.create(getMemoryPool(), inf.m_msg);
	newLogItem->m_line = inf.m_line;
	newLogItem->m_type = inf.m_type;

	// Push it back
	m_logItems.pushBack(newLogItem);
	++m_logItemCount;

	m_logItemsTimestamp.fetchAdd(1);
}

} // end namespace anki
