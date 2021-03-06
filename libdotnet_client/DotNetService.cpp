#include "Destination.h"
#include "Identity.h"
#include "ClientContext.h"
#include "DotNetService.h"
#include <boost/asio/error.hpp>

namespace dotnet
{
namespace client
{
	static const dotnet::data::SigningKeyType DOTNET_SERVICE_DEFAULT_KEY_TYPE = dotnet::data::SIGNING_KEY_TYPE_EDDSA_SHA512_ED25519;

	DotNetService::DotNetService (std::shared_ptr<ClientDestination> localDestination):
		m_LocalDestination (localDestination ? localDestination :
			dotnet::client::context.CreateNewLocalDestination (false, DOTNET_SERVICE_DEFAULT_KEY_TYPE)),
			m_ReadyTimer(m_LocalDestination->GetService()),
			m_ReadyTimerTriggered(false),
			m_ConnectTimeout(0),
			isUpdated (true)
	{
		m_LocalDestination->Acquire ();
	}

	DotNetService::DotNetService (dotnet::data::SigningKeyType kt):
		m_LocalDestination (dotnet::client::context.CreateNewLocalDestination (false, kt)),
		m_ReadyTimer(m_LocalDestination->GetService()),
		m_ConnectTimeout(0),
		isUpdated (true)
	{
		m_LocalDestination->Acquire ();
	}

	DotNetService::~DotNetService ()
	{
		ClearHandlers ();
		if (m_LocalDestination) m_LocalDestination->Release ();
	}

	void DotNetService::ClearHandlers ()
	{
		if(m_ConnectTimeout)
			m_ReadyTimer.cancel();
		std::unique_lock<std::mutex> l(m_HandlersMutex);
		for (auto it: m_Handlers)
			it->Terminate ();
		m_Handlers.clear();
	}

	void DotNetService::SetConnectTimeout(uint32_t timeout)
	{
		m_ConnectTimeout = timeout;
	}

	void DotNetService::AddReadyCallback(ReadyCallback cb)
	{
		uint32_t now = dotnet::util::GetSecondsSinceEpoch();
		uint32_t tm = (m_ConnectTimeout) ? now + m_ConnectTimeout : NEVER_TIMES_OUT;

		LogPrint(eLogDebug, "DotNetService::AddReadyCallback() ", tm, " ", now);
		m_ReadyCallbacks.push_back({cb, tm});
		if (!m_ReadyTimerTriggered) TriggerReadyCheckTimer();
	}

	void DotNetService::TriggerReadyCheckTimer()
	{
		m_ReadyTimer.expires_from_now(boost::posix_time::seconds (1));
		m_ReadyTimer.async_wait(std::bind(&DotNetService::HandleReadyCheckTimer, shared_from_this (), std::placeholders::_1));
		m_ReadyTimerTriggered = true;

	}

	void DotNetService::HandleReadyCheckTimer(const boost::system::error_code &ec)
	{
		if(ec || m_LocalDestination->IsReady())
		{
			for(auto & itr : m_ReadyCallbacks)
				itr.first(ec);
			m_ReadyCallbacks.clear();
		}
		else if(!m_LocalDestination->IsReady())
		{
			// expire timed out requests
			uint32_t now = dotnet::util::GetSecondsSinceEpoch ();
			auto itr = m_ReadyCallbacks.begin();
			while(itr != m_ReadyCallbacks.end())
			{
			    if(itr->second != NEVER_TIMES_OUT && now >= itr->second)
				{
					itr->first(boost::asio::error::timed_out);
					itr = m_ReadyCallbacks.erase(itr);
				}
				else
					++itr;
			}
		}
		if(!ec && m_ReadyCallbacks.size())
		    TriggerReadyCheckTimer();
		else
		    m_ReadyTimerTriggered = false;
	}

	void DotNetService::CreateStream (StreamRequestComplete streamRequestComplete, const std::string& dest, int port) {
		assert(streamRequestComplete);
		auto address = dotnet::client::context.GetAddressBook ().GetAddress (dest);
		if (address)
			CreateStream(streamRequestComplete, address, port);
		else
		{
			LogPrint (eLogWarning, "DotNetService: Remote destination not found: ", dest);
			streamRequestComplete (nullptr);
		}
	}

	void DotNetService::CreateStream(StreamRequestComplete streamRequestComplete, std::shared_ptr<const Address> address, int port)
	{
		if(m_ConnectTimeout && !m_LocalDestination->IsReady())
		{
			AddReadyCallback([this, streamRequestComplete, address, port] (const boost::system::error_code & ec) {
					if(ec)
					{
						LogPrint(eLogWarning, "DotNetService::CeateStream() ", ec.message());
						streamRequestComplete(nullptr);
					}
					else
					{	if (address->IsIdentHash ())
							this->m_LocalDestination->CreateStream(streamRequestComplete, address->identHash, port);
						else
							this->m_LocalDestination->CreateStream (streamRequestComplete, address->blindedPublicKey, port);	
					}
				});
		}
		else
		{	
			if (address->IsIdentHash ())
				m_LocalDestination->CreateStream (streamRequestComplete, address->identHash, port);
			else
				m_LocalDestination->CreateStream (streamRequestComplete, address->blindedPublicKey, port);
		}
	}

	TCPIPPipe::TCPIPPipe(DotNetService * owner, std::shared_ptr<boost::asio::ip::tcp::socket> upstream, std::shared_ptr<boost::asio::ip::tcp::socket> downstream) : DotNetServiceHandler(owner), m_up(upstream), m_down(downstream)
	{
		boost::asio::socket_base::receive_buffer_size option(TCP_IP_PIPE_BUFFER_SIZE);
		upstream->set_option(option);
		downstream->set_option(option);
	}

	TCPIPPipe::~TCPIPPipe()
	{
		Terminate();
	}

	void TCPIPPipe::Start()
	{
		AsyncReceiveUpstream();
		AsyncReceiveDownstream();
	}

	void TCPIPPipe::Terminate()
	{
		if(Kill()) return;
		if (m_up)
		{
			if (m_up->is_open())
				m_up->close();
			m_up = nullptr;
		}
		if (m_down)
		{
			if (m_down->is_open())
				m_down->close();
			m_down = nullptr;
		}
		Done(shared_from_this());
	}

	void TCPIPPipe::AsyncReceiveUpstream()
	{
		if (m_up)
		{
			m_up->async_read_some(boost::asio::buffer(m_upstream_to_down_buf, TCP_IP_PIPE_BUFFER_SIZE),
				std::bind(&TCPIPPipe::HandleUpstreamReceived, shared_from_this(),
				std::placeholders::_1, std::placeholders::_2));
		}
		else
			LogPrint(eLogError, "TCPIPPipe: upstream receive: no socket");
	}

	void TCPIPPipe::AsyncReceiveDownstream()
	{
		if (m_down) {
			m_down->async_read_some(boost::asio::buffer(m_downstream_to_up_buf, TCP_IP_PIPE_BUFFER_SIZE),
				std::bind(&TCPIPPipe::HandleDownstreamReceived, shared_from_this(),
				std::placeholders::_1, std::placeholders::_2));
		}
		else
			LogPrint(eLogError, "TCPIPPipe: downstream receive: no socket");
	}

	void TCPIPPipe::UpstreamWrite(size_t len)
	{
		if (m_up)
		{
			LogPrint(eLogDebug, "TCPIPPipe: upstream: ", (int) len, " bytes written");
			boost::asio::async_write(*m_up, boost::asio::buffer(m_upstream_buf, len),
				boost::asio::transfer_all(),
				std::bind(&TCPIPPipe::HandleUpstreamWrite,
				shared_from_this(),
				std::placeholders::_1));
		}
		else
			LogPrint(eLogError, "TCPIPPipe: upstream write: no socket");
	}

	void TCPIPPipe::DownstreamWrite(size_t len)
	{
		if (m_down)
		{
			LogPrint(eLogDebug, "TCPIPPipe: downstream: ", (int) len, " bytes written");
			boost::asio::async_write(*m_down, boost::asio::buffer(m_downstream_buf, len),
				boost::asio::transfer_all(),
				std::bind(&TCPIPPipe::HandleDownstreamWrite,
				shared_from_this(),
				std::placeholders::_1));
		}
		else
			LogPrint(eLogError, "TCPIPPipe: downstream write: no socket");
	}


	void TCPIPPipe::HandleDownstreamReceived(const boost::system::error_code & ecode, std::size_t bytes_transfered)
	{
		LogPrint(eLogDebug, "TCPIPPipe: downstream: ", (int) bytes_transfered, " bytes received");
		if (ecode)
		{
			LogPrint(eLogError, "TCPIPPipe: downstream read error:" , ecode.message());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate();
		} else {
			if (bytes_transfered > 0 )
				memcpy(m_upstream_buf, m_downstream_to_up_buf, bytes_transfered);
			UpstreamWrite(bytes_transfered);
		}
	}

	void TCPIPPipe::HandleDownstreamWrite(const boost::system::error_code & ecode) {
		if (ecode)
		{
			LogPrint(eLogError, "TCPIPPipe: downstream write error:" , ecode.message());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate();
		}
		else
			AsyncReceiveUpstream();
	}

	void TCPIPPipe::HandleUpstreamWrite(const boost::system::error_code & ecode) {
		if (ecode)
		{
			LogPrint(eLogError, "TCPIPPipe: upstream write error:" , ecode.message());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate();
		}
		else
			AsyncReceiveDownstream();
	}

	void TCPIPPipe::HandleUpstreamReceived(const boost::system::error_code & ecode, std::size_t bytes_transfered)
	{
		LogPrint(eLogDebug, "TCPIPPipe: upstream ", (int)bytes_transfered, " bytes received");
		if (ecode)
		{
			LogPrint(eLogError, "TCPIPPipe: upstream read error:" , ecode.message());
			if (ecode != boost::asio::error::operation_aborted)
				Terminate();
		} else {
			if (bytes_transfered > 0 )
				memcpy(m_downstream_buf, m_upstream_to_down_buf, bytes_transfered);
			DownstreamWrite(bytes_transfered);
		}
	}

	void TCPIPAcceptor::Start ()
	{
		m_Acceptor.reset (new boost::asio::ip::tcp::acceptor (GetService (), m_LocalEndpoint));
		//update the local end point in case port has been set zero and got updated now
		m_LocalEndpoint = m_Acceptor->local_endpoint();
		m_Acceptor->listen ();
		Accept ();
	}

	void TCPIPAcceptor::Stop ()
	{
		if (m_Acceptor)
		{
			m_Acceptor->close();
			m_Acceptor.reset (nullptr);
		}
		m_Timer.cancel ();
		ClearHandlers();
	}

	void TCPIPAcceptor::Accept ()
	{
		auto newSocket = std::make_shared<boost::asio::ip::tcp::socket> (GetService ());
		m_Acceptor->async_accept (*newSocket, std::bind (&TCPIPAcceptor::HandleAccept, this,
			std::placeholders::_1, newSocket));
	}

	void TCPIPAcceptor::HandleAccept (const boost::system::error_code& ecode, std::shared_ptr<boost::asio::ip::tcp::socket> socket)
	{
		if (!ecode)
		{
			LogPrint(eLogDebug, "DotNetService: ", GetName(), " accepted");
			auto handler = CreateHandler(socket);
			if (handler)
			{
				AddHandler(handler);
				handler->Handle();
			}
			else
				socket->close();
			Accept();
		}
		else
		{
			if (ecode != boost::asio::error::operation_aborted)
				LogPrint (eLogError, "DotNetService: ", GetName(), " closing socket on accept because: ", ecode.message ());
		}
	}
}
}
