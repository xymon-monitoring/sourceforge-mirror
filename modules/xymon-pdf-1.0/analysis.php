<!DOCTYPE HTML PUBLIC "-//W3C//DTD HTML 4.0 Transitional//EN">
<HTML>
<HEAD>
<TITLE>Xymon : PDF analysis</TITLE>

<!-- Styles for the menu bar -->
<link rel="stylesheet" type="text/css" href="/hobbit/menu/menu.css">
<meta http-equiv="refresh" content="600">

<SCRIPT language="JavaScript">
<!--   // if your browser doesn't support Javascript, following lines will be hidden.

var loadavg_good_text="Enter a text to describe CPU load."
var memory_good_text="Enter a text to describe Memory Utilization."
var network_good_text="Enter a text to describe Network Traffic."
var runqueue_good_text="Enter a text to describe Run-queue Length."
var iowait_good_text="Enter a text to describe I/O wait Rate." 
var vmstat_good_text="Enter a text to describe CPU Utilization."
var conclusion_good_text="What is your conclusion ?"

var loadavg_bad_text="Enter a text to describe CPU load."
var memory_bad_text="Enter a text to describe Memory Utilization."
var network_bad_text="Enter a text to describe Network Traffic."
var runqueue_bad_text="Enter a text to describe Run-queue Length."
var iowait_bad_text="Enter a text to describe I/O wait Rate."
var vmstat_bad_text="Enter a text to describe CPU Utilization."
var conclusion_bad_text="What is your conclusion ?"

var loadavg_ugly_text="Enter a text to describe CPU load."
var memory_ugly_text="Enter a text to describe Memory Utilization."
var network_ugly_text="Enter a text to describe Network Traffic."
var runqueue_ugly_text="Enter a text to describe Run-queue Length."
var iowait_ugly_text="Enter a text to describe I/O wait Rate."
var vmstat_ugly_text="Enter a text to describe CPU Utilization."
var conclusion_ugly_text="What is your conclusion ?"

/*var loadavg_good_text="La charge moyenne est comprise entre 0.5 et 1. Le serveur est donc sollicit� r�guli�rement mais pas en permanence. Eventuellement, de nouvelles applications pourront �tre h�berg�es. Nous sommes donc dans une bonne configuration. Le serveur est adapt� en terme de puissance CPU." ;
var memory_good_text="La m�moire physique est utilis�e � 80% en moyenne et atteint les 100% en pic. C'est une configuration correcte. Idem pour l'utilisation de la swap : son utilisation ne d�passe pas la taille de la m�moire physique, il y a donc suffisament de RAM dans le serveur. De nouvelles applications peuvent �tre install�es." ;
var network_good_text="Pas de traffic important et pas d'utilisation intensive sur une longue p�riode de temps. Configuration optimale." ;
var runqueue_good_text="La run-queue �gale le nombre de processeurs pr�sents dans le serveur (X). Le syst�me n'a donc pas une charge de travail lourde." ;
var iowait_good_text="Le taux d'I/O wait, en moyenne, ne d�passe pas les 15%. Le serveur ne perd donc pas de temps en attente d'I/O. Il n'y a pas de probl�mes sur les disques du syst�me. " ;
var vmstat_good_text="Le temps d�di� au syst�me est inf�rieur � 15% donc le nombre d'appels syst�mes ou de demandes d'I/O est correct. Le temps utilisateur (pour les applications) est inf�rieur � 90% ce qui signifie que le serveur peut accueillir d'autres applications. Le temps d'inactivit� est bon puisque compris entre 40 et 90%. Le serveur passe donc une partie de son temps � attendre des demandes de traitements." ;
var conclusion_good_text="Apr�s analyse de ces graphes, nous pouvons dire que le serveur est adapt� pour les applications et les traitements qu'il h�berge actuellement. Il pourra m�me accueillir d'autres applications si n�cessaire. Il faudra faire attention � ne pas installer ou migrer des applications trop gourmandes sous peine de voir les performances s'effondrer. Le serveur est bien dimensionn�." ;

var loadavg_bad_text="La charge moyenne est inf�rieure � 0.5 en moyenne. Quelques pics atteignent 1 d�montrant que la machine est sollicit�e par moment. Nous sommes donc dans une configuration correcte en terme de puissance puisque le serveur pourra g�rer sans probl�mes des pics de charge impr�vus. Par contre, d'un point de vue financier, le rapport puissance/prix n'est pas bon du tout. Le serveur est largement sous-utilis�. En terme de puissance CPU, d'apr�s ce graphe, nous sommes dans un cas de sur-dimensionnement. Afin de rentabiliser au mieux la machine, il serait envisageable d'y mettre de nouvelles applications." ;
var memory_bad_text="La m�moire physique est utilis�e � 50% en moyenne. Une fois de plus, il s'agit d'une configuration correcte. Idem pour l'utilisation de la swap. L'espace r�serv� pour son utilisation n'est pas occup� ou exceptionnellement." ;
var network_bad_text="Pas de traffic important et pas d'utilisation intensive sur une longue p�riode de temps. Configuration optimale." ;
var runqueue_bad_text="La run-queue est inf�rieure � 1 et ne d�passe que tr�s peu fr�quemment cette valeur. Comme nous l'avons d�j� vu avec les autres graphes, le syst�me n'a pas une charge de travail lourde. Elle est m�me quasiment nulle." ;
var iowait_bad_text="Pas ou peu de valeurs au-dessus de 0%. Le serveur ne perd donc pas de temps en attente d'I/O et r�pond de mani�re optimale." ;
var vmstat_bad_text="Le temps d�di� au syst�me est inf�rieur � 10% en moyenne donc le nombre d'appels syst�mes ou de demandes d'I/O est correct voire inexistant. Le temps utilisateur (devou� aux applications) est inf�rieur � 10%, ce qui signifie que le serveur peut accueillir sans soucis d'autres applications. Le temps d'inactivit� est maximal puisque compris entre 90 et 100%." ;
var conclusion_bad_text="Apr�s analyse de ces graphes, nous pouvons dire que le serveur est clairement adapt� en terme de puissance puisqu'il ne travaille quasiment jamais ! Un serveur co�tant une somme d'argent non-n�gligeable, il est recommand� de mettre les nouvelles applications sur cette machine en priorit�. De m�me, pour des applications devant migrer, ce serveur pourra les accueillir sans probl�mes. Le serveur est sur-dimensionn�." ;

var loadavg_ugly_text="La charge moyenne est tr�s sup�rieure � 1. Nous sommes donc dans une position d�licate : le serveur est inadapt� en terme de puissance CPU. Ce qui signifie qu'il faudra certainement rajouter des processeurs ou pire, migrer certaines applications vers d'autres serveurs moins charg�s. Les cinq autres graphes vont nous aider � confirmer cette tendance." ;
var memory_ugly_text="La m�moire physique est utilis�e � 100% en moyenne. L'espace de swap est utilis� � plus de 50% et d�passe la taille totale de m�moire physique pr�sente dans le syst�me. Si la swap atteint �galement les 100%, le syst�me d'exploitation refusera l'ex�cution de nouvelles applications et pourra, dans le pire des cas, provoquer un red�marrage inopin� du syst�me. Des applications r�agissant de mani�re incoh�rente ou impr�vue sont �galement � pr�voir dans une telle situation." ;
var network_ugly_text="Traffic important et utilisation intensive sur une longue p�riode de temps. V�rifier qu'aucune application ne s'est plant�e et ne provoque de charge r�seau importante." ;
var runqueue_ugly_text="La run-queue est sup�rieure au nombre de processeurs (si Solaris sup�rieure � trois fois le nombre de CPUs) pr�sents dans la machine (X). Le syst�me a donc une charge de travail trop lourde." ;
var iowait_ugly_text="Pics fr�quents au-dessus de 30% et la moyenne est comprise entre 15 et 30%. Le serveur perd donc un temps consid�rable en attente d'I/O. Ceci est probablement li� au ph�nom�ne de swap qui provoque une utilisation intensive des disques. En cons�quence, le syst�me est beaucoup plus lent � r�agir." ;
var vmstat_ugly_text="Le temps d�di� au syst�me est sup�rieur � 10%, cela confirme que le nombre d'appels syst�mes ou de demandes d'I/O est tr�s �lev�. Le temps utilisateur (pour les applications) atteint r�guli�rement ou continuellement 100%. Le serveur ne peut plus accueillir d'autres applications. Le temps d'inactivit� est ex�crable puisque compris entre 0 et 10%. Le syst�me n'arrive pas � d�gager suffisament de temps pour traiter toutes les applications." ;
var conclusion_ugly_text="Apr�s analyse de ces graphes, nous pouvons dire que le serveur n'a pas la puissance requise pour la charge de travail impartie. L'urgence de la situation est r�elle et il est clairement le temps de r�fl�chir � un plan de migration de certaines applications vers d'autres serveurs moins charg�s ou de r�aliser un upgrade mat�riel, comme un ajout de RAM et/ou de CPUs. Dans tous les cas, ce serveur est d�sormais dangereux pour les applications qu'il h�berge car les temps de r�ponse, de traitement et de disponibilit� ne sont tout simplement plus garantis. Le serveur est sous-dimensionn�." ;
*/

//-->  // End of hidden part
</SCRIPT>

</HEAD>

<BODY BGCOLOR="red" BACKGROUND="/hobbit/gifs/bkg-blue.gif" TEXT="#D8D8BF" LINK="#00FFAA" VLINK="#FFFF44">

<TABLE SUMMARY="Topline" WIDTH="100%">
<TR><TD HEIGHT=16>&nbsp;</TD></TR>  <!-- For the menu bar -->
<TR>
  <TD VALIGN=MIDDLE ALIGN=LEFT WIDTH="30%">
    <FONT FACE="Arial, Helvetica" SIZE="+1" COLOR="silver"><B>Xymon</B></FONT>
  </TD>
  <TD VALIGN=MIDDLE ALIGN=CENTER WIDTH="40%">
    <CENTER><FONT FACE="Arial, Helvetica" SIZE="+1" COLOR="silver"><B>PDF Analysis</B></FONT></CENTER>
  </TD>
  <TD VALIGN=MIDDLE ALIGN=RIGHT WIDTH="30%">
   <FONT FACE="Arial, Helvetica" SIZE="+1" COLOR="silver">
     <B>
	<? 
	// setlocale(LC_ALL, "fr_FR");
	echo htmlentities(strftime("%A %d %B %T")) ; 
	?>
     </B>
   </FONT>
  </TD>
</TR>
<TR>
  <TD COLSPAN=3> <HR WIDTH="100%"> </TD>
</TR>
</TABLE>

<BR>


<center>

<form name="selprj" action="report.php" method="post">

Hostname :
<INPUT type="text" name="nom">

<BR><BR>

<LABEL for='loaded'><INPUT type="radio" name="verif" id='loaded' value=ok  onClick=" this.form.loadavg.value=loadavg_good_text ; this.form.memory.value=memory_good_text ; this.form.runqueue.value=runqueue_good_text ; this.form.network.value=network_good_text ; this.form.iowait.value=iowait_good_text ; this.form.vmstat.value=vmstat_good_text ; this.form.conclusion.value=conclusion_good_text " checked>Loaded</LABEL>
<LABEL for='uloaded'><INPUT type="radio" name="verif" id='uloaded' value=mok  onClick=" this.form.loadavg.value=loadavg_bad_text ; this.form.memory.value=memory_bad_text ; this.form.runqueue.value=runqueue_bad_text ; this.form.network.value=network_bad_text ; this.form.iowait.value=iowait_bad_text ; this.form.vmstat.value=vmstat_bad_text ; this.form.conclusion.value=conclusion_bad_text ">Under-loaded</LABEL>
<LABEL for='hloaded'><INPUT type="radio" name="verif" id='hloaded' value=nok  onClick=" this.form.loadavg.value=loadavg_ugly_text ; this.form.memory.value=memory_ugly_text ; this.form.runqueue.value=runqueue_ugly_text ; this.form.network.value=network_ugly_text ; this.form.iowait.value=iowait_ugly_text ; this.form.vmstat.value=vmstat_ugly_text ; this.form.conclusion.value=conclusion_ugly_text ">Heavy-loaded</LABEL>

<BR><BR>

<textarea name="loadavg" COLS=50 ROWS=5 WRAP=physical>CPU Load</TEXTAREA>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
<textarea name="memory" COLS=50 ROWS=5 WRAP=physical>Memory</TEXTAREA>
<BR>
<textarea name="runqueue" COLS=50 ROWS=5 WRAP=physical>Run-queue</TEXTAREA>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
<textarea name="network" COLS=50 ROWS=5 WRAP=physical>Network Traffic</TEXTAREA>
<BR>
<textarea name="iowait" COLS=50 ROWS=5 WRAP=physical>I/O wait</TEXTAREA>
&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;&nbsp;
<textarea name="vmstat" COLS=50 ROWS=5 WRAP=physical>CPU Utilization</TEXTAREA>
<BR><BR>
<textarea name="conclusion" COLS=50 ROWS=5 WRAP=physical>Conclusion</TEXTAREA>
<BR>
<BR>

<INPUT type="hidden" name="chxrap" value="analyse">
<input type="submit" name="analyse" value="Report !">
</form> 


</center>


<TABLE SUMMARY="Bottomline" WIDTH="100%">
<TR>
  <TD> <HR WIDTH="100%"> </TD>
</TR>
<TR>
  <TD ALIGN=RIGHT><FONT FACE="Arial, Helvetica" SIZE="-2" COLOR="silver"><B><A HREF="http://hobbitmon.sourceforge.net/" style="text-decoration: none">Xymon</A></B></FONT></TD>
</TR>
</TABLE>


<!-- menu script itself. you should not modify this file -->
<script type="text/javascript" language="JavaScript" src="/hobbit/menu/menu.js"></script>
<!-- items structure. menu hierarchy and links are stored there -->
<script type="text/javascript" language="JavaScript" src="/hobbit/menu/menu_items.js"></script>
<!-- files with geometry and styles structures -->
<script type="text/javascript" language="JavaScript" src="/hobbit/menu/menu_tpl.js"></script>
<script type="text/javascript" language="JavaScript">
        new menu (MENU_ITEMS, MENU_POS);
</script>

</BODY>
</HTML>
