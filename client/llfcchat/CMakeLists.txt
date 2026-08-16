cmake_minimum_required(VERSION 3.25)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  if(NOT CMAKE_GENERATOR STREQUAL "Ninja Multi-Config")
    message(FATAL_ERROR
      "This project requires the \"Ninja Multi-Config\" generator. "
      "Configure with: cmake -G \"Ninja Multi-Config\" ...")
  endif()
  if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "In-source builds are not supported. Use a separate build directory.")
  endif()
  get_filename_component(_llfc_repo_root "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
  set(VCPKG_MANIFEST_DIR "${_llfc_repo_root}" CACHE PATH "vcpkg manifest directory" FORCE)
  set(VCPKG_INSTALLED_DIR "${_llfc_repo_root}/vcpkg_installed" CACHE PATH "vcpkg installed directory" FORCE)
  set(LLFC_REPO_ROOT "${_llfc_repo_root}" CACHE PATH "llfcchat repository root" FORCE)

  project(llfcchat LANGUAGES CXX RC)

  list(APPEND CMAKE_MODULE_PATH "${LLFC_REPO_ROOT}/cmake")
  include(Dependencies)

  set(CMAKE_CXX_STANDARD 11)
  set(CMAKE_CXX_STANDARD_REQUIRED ON)
  set(CMAKE_CXX_EXTENSIONS OFF)
else()
  # Aggregated from repo root — ensure RC language for .rc embedding under Ninja.
  enable_language(RC)
  if(NOT DEFINED LLFC_REPO_ROOT)
    get_filename_component(LLFC_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
    set(LLFC_REPO_ROOT "${LLFC_REPO_ROOT}" CACHE PATH "llfcchat repository root" FORCE)
  endif()
endif()

llfc_find_client_dependencies()

set(LLFCCHAT_SOURCES
  src/main.cpp
  src/app/mainwindow.cpp
  src/app/chatdialog.cpp
  src/app/userinfopage.cpp
  src/app/friendinfopage.cpp
  src/dialogs/logindialog.cpp
  src/dialogs/registerdialog.cpp
  src/dialogs/resetdialog.cpp
  src/dialogs/applyfriend.cpp
  src/dialogs/authenfriend.cpp
  src/dialogs/findfaildlg.cpp
  src/dialogs/findsuccessdlg.cpp
  src/dialogs/loadingdlg.cpp
  src/dialogs/friendlabel.cpp
  src/dialogs/imagecropperlabel.cpp
  src/chat/chatpage.cpp
  src/chat/ChatView.cpp
  src/chat/ChatItemBase.cpp
  src/chat/BubbleFrame.cpp
  src/chat/TextBubble.cpp
  src/chat/PictureBubble.cpp
  src/chat/FileBubble.cpp
  src/chat/ClickableLabel.cpp
  src/chat/MessageTextEdit.cpp
  src/contact/chatuserlist.cpp
  src/contact/contactuserlist.cpp
  src/contact/searchlist.cpp
  src/contact/applyfriendlist.cpp
  src/contact/applyfriendpage.cpp
  src/contact/listitembase.cpp
  src/contact/chatuserwid.cpp
  src/contact/conuseritem.cpp
  src/contact/adduseritem.cpp
  src/contact/applyfrienditem.cpp
  src/contact/grouptipitem.cpp
  src/contact/lineitem.cpp
  src/contact/invaliditem.cpp
  src/widgets/clickedbtn.cpp
  src/widgets/clickedlabel.cpp
  src/widgets/clickedoncelabel.cpp
  src/widgets/customizeedit.cpp
  src/widgets/customizetextedit.cpp
  src/widgets/statelabel.cpp
  src/widgets/statewidget.cpp
  src/widgets/timerbtn.cpp
  src/net/tcpmgr.cpp
  src/net/httpmgr.cpp
  src/net/filetcpmgr.cpp
  src/store/usermgr.cpp
  src/store/localchatdb.cpp
  src/store/localchatstore.cpp
  src/store/chatsyncmanager.cpp
  src/store/outboxdispatcher.cpp
  src/store/userdata.cpp
  src/protocol/global.cpp
)

set(LLFCCHAT_HEADERS
  src/app/mainwindow.h
  src/app/chatdialog.h
  src/app/userinfopage.h
  src/app/friendinfopage.h
  src/dialogs/logindialog.h
  src/dialogs/registerdialog.h
  src/dialogs/resetdialog.h
  src/dialogs/applyfriend.h
  src/dialogs/authenfriend.h
  src/dialogs/findfaildlg.h
  src/dialogs/findsuccessdlg.h
  src/dialogs/loadingdlg.h
  src/dialogs/friendlabel.h
  src/dialogs/imagecropperdialog.h
  src/dialogs/imagecropperlabel.h
  src/chat/chatpage.h
  src/chat/ChatView.h
  src/chat/ChatItemBase.h
  src/chat/BubbleFrame.h
  src/chat/TextBubble.h
  src/chat/PictureBubble.h
  src/chat/FileBubble.h
  src/chat/ClickableLabel.h
  src/chat/MessageTextEdit.h
  src/contact/chatuserlist.h
  src/contact/contactuserlist.h
  src/contact/searchlist.h
  src/contact/applyfriendlist.h
  src/contact/applyfriendpage.h
  src/contact/listitembase.h
  src/contact/chatuserwid.h
  src/contact/conuseritem.h
  src/contact/adduseritem.h
  src/contact/applyfrienditem.h
  src/contact/grouptipitem.h
  src/contact/lineitem.h
  src/contact/invaliditem.h
  src/widgets/clickedbtn.h
  src/widgets/clickedlabel.h
  src/widgets/clickedoncelabel.h
  src/widgets/customizeedit.h
  src/widgets/customizetextedit.h
  src/widgets/statelabel.h
  src/widgets/statewidget.h
  src/widgets/timerbtn.h
  src/net/tcpmgr.h
  src/net/httpmgr.h
  src/net/filetcpmgr.h
  src/store/usermgr.h
  src/store/localchatdb.h
  src/store/localchatstore.h
  src/store/chatsyncmanager.h
  src/store/outboxdispatcher.h
  src/store/userdata.h
  src/store/localmessageDTO.h
  src/protocol/global.h
  src/util/singleton.h
)

set(LLFCCHAT_UIS
  src/app/mainwindow.ui
  src/app/chatdialog.ui
  src/app/userinfopage.ui
  src/app/friendinfopage.ui
  src/dialogs/logindialog.ui
  src/dialogs/registerdialog.ui
  src/dialogs/resetdialog.ui
  src/dialogs/applyfriend.ui
  src/dialogs/authenfriend.ui
  src/dialogs/findfaildlg.ui
  src/dialogs/findsuccessdlg.ui
  src/dialogs/loadingdlg.ui
  src/dialogs/friendlabel.ui
  src/chat/chatpage.ui
  src/contact/applyfriendpage.ui
  src/contact/chatuserwid.ui
  src/contact/conuseritem.ui
  src/contact/adduseritem.ui
  src/contact/applyfrienditem.ui
  src/contact/grouptipitem.ui
  src/contact/lineitem.ui
)

add_executable(llfcchat WIN32
  ${LLFCCHAT_SOURCES}
  ${LLFCCHAT_HEADERS}
  ${LLFCCHAT_UIS}
  resources/rc.qrc
  resources/llfcchat.rc
)

set_target_properties(llfcchat PROPERTIES
  AUTOMOC ON
  AUTOUIC ON
  AUTORCC ON
  CXX_STANDARD 11
  CXX_STANDARD_REQUIRED ON
  CXX_EXTENSIONS OFF
)

# 每个 .ui 与引用它的 .cpp 同目录，AUTOUIC 按同目录查找即可，
# 各功能子目录都加入 include 路径，保持既有裸文件名 #include 不变。
target_include_directories(llfcchat PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src/app"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/dialogs"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/chat"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/contact"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/widgets"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/net"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/store"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/protocol"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/util"
)

target_compile_definitions(llfcchat PRIVATE QT_DEPRECATED_WARNINGS)

target_link_libraries(llfcchat PRIVATE
  Qt5::Core
  Qt5::Gui
  Qt5::Network
  Qt5::Sql
  Qt5::Widgets
  Qt5::WinMain
)

# Static plugin import for platforms / imageformats / styles (qt5-base + qt5-imageformats).
qt5_import_plugins(llfcchat
  INCLUDE_BY_TYPE platforms Qt5::QWindowsIntegrationPlugin
  INCLUDE_BY_TYPE imageformats
    Qt5::QGifPlugin
    Qt5::QJpegPlugin
    Qt5::QWebpPlugin
  INCLUDE_BY_TYPE styles Qt5::QWindowsVistaStylePlugin
  INCLUDE_BY_TYPE sqldrivers Qt5::QSQLiteDriverPlugin
)

llfc_apply_msvc_defaults(llfcchat)
llfc_set_runtime_outdir(llfcchat llfcchat)

add_custom_command(TARGET llfcchat POST_BUILD
  COMMAND "${CMAKE_COMMAND}" -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/config/config.ini"
    "$<TARGET_FILE_DIR:llfcchat>/config.ini"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/static"
    "$<TARGET_FILE_DIR:llfcchat>/static"
  COMMENT "Deploy llfcchat config.ini and static assets"
  VERBATIM
)
